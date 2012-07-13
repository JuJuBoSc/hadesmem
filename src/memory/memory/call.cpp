// Copyright Joshua Boyce 2010-2012.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
// This file is part of HadesMem.
// <http://www.raptorfactor.com/> <raptorfactor@raptorfactor.com>

#include "hadesmem/call.hpp"

#include "hadesmem/detail/warning_disable_prefix.hpp"
#include <boost/scope_exit.hpp>
#include "hadesmem/detail/warning_disable_suffix.hpp"

#include "hadesmem/detail/warning_disable_prefix.hpp"
#include <AsmJit/AsmJit.h>
#include "hadesmem/detail/warning_disable_suffix.hpp"

#include "hadesmem/read.hpp"
#include "hadesmem/alloc.hpp"
#include "hadesmem/error.hpp"
#include "hadesmem/write.hpp"
#include "hadesmem/module.hpp"
#include "hadesmem/process.hpp"

// TODO: Rewrite, clean up, etc...
// TODO: Split code gen into detail funcs etc.

namespace hadesmem
{

RemoteFunctionRet::RemoteFunctionRet(DWORD_PTR ReturnValue, 
  DWORD64 ReturnValue64, DWORD LastError)
  : m_ReturnValue(ReturnValue), 
  m_ReturnValue64(ReturnValue64), 
  m_LastError(LastError)
{ }

DWORD_PTR RemoteFunctionRet::GetReturnValue() const
{
  return m_ReturnValue;
}

DWORD64 RemoteFunctionRet::GetReturnValue64() const
{
  return m_ReturnValue64;
}

DWORD RemoteFunctionRet::GetLastError() const
{
  return m_LastError;
}

RemoteFunctionRet Call(Process const& process, 
  LPCVOID address, 
  CallConv call_conv, 
  std::vector<PVOID> const& args)
{
  std::vector<LPCVOID> addresses;
  addresses.push_back(address);
  std::vector<CallConv> call_convs;
  call_convs.push_back(call_conv);
  std::vector<std::vector<PVOID>> args_full;
  args_full.push_back(args);
  return CallMulti(process, addresses, call_convs, args_full)[0];
}

std::vector<RemoteFunctionRet> CallMulti(Process const& process, 
  std::vector<LPCVOID> addresses, 
  std::vector<CallConv> call_convs, 
  std::vector<std::vector<PVOID>> const& args_full) 
{
  if (addresses.size() != call_convs.size() || 
    addresses.size() != args_full.size())
  {
    BOOST_THROW_EXCEPTION(HadesMemError() << 
      ErrorString("Size mismatch in parameters."));
  }
  
  Allocator const return_value_remote(process, sizeof(DWORD_PTR) * 
    addresses.size());
  Allocator const return_value_64_remote(process, sizeof(DWORD64) * 
    addresses.size());
  Allocator const last_error_remote(process, sizeof(DWORD) * 
    addresses.size());

  Module kernel32(&process, L"kernel32.dll");
  DWORD_PTR const get_last_error = reinterpret_cast<DWORD_PTR>(
    FindProcedure(kernel32, "GetLastError"));
  DWORD_PTR const set_last_error = reinterpret_cast<DWORD_PTR>(
    FindProcedure(kernel32, "SetLastError"));
  
  AsmJit::Assembler assembler;
  
#if defined(_M_AMD64)
  for (std::size_t i = 0; i < addresses.size(); ++i)
  {
    LPCVOID address = addresses[i];
    CallConv call_conv = call_convs[i];
    std::vector<PVOID> const& args = args_full[i];
    std::size_t const num_args = args.size();
    
    // Check calling convention
    if (call_conv != CallConv::kX64 && 
      call_conv != CallConv::kDefault)
    {
      BOOST_THROW_EXCEPTION(HadesMemError() << 
        ErrorString("Invalid calling convention."));
    }
    
    // Prologue
    assembler.push(AsmJit::rbp);
    assembler.mov(AsmJit::rbp, AsmJit::rsp);

    // Allocate ghost space
    assembler.sub(AsmJit::rsp, AsmJit::Imm(0x20));

    // Call kernel32.dll!SetLastError
    assembler.mov(AsmJit::rcx, 0);
    assembler.mov(AsmJit::rax, set_last_error);
    assembler.call(AsmJit::rax);

    // Cleanup ghost space
    assembler.add(AsmJit::rsp, AsmJit::Imm(0x20));

    // Set up first 4 parameters
    assembler.mov(AsmJit::rcx, num_args > 0 ? reinterpret_cast<DWORD_PTR>(
      args[0]) : 0);
    assembler.mov(AsmJit::rdx, num_args > 1 ? reinterpret_cast<DWORD_PTR>(
      args[1]) : 0);
    assembler.mov(AsmJit::r8, num_args > 2 ? reinterpret_cast<DWORD_PTR>(
      args[2]) : 0);
    assembler.mov(AsmJit::r9, num_args > 3 ? reinterpret_cast<DWORD_PTR>(
      args[3]) : 0);

    // Handle remaining parameters (if any)
    if (num_args > 4)
    {
      std::for_each(args.crbegin(), args.crend() - 4, 
        [&assembler] (PVOID arg)
      {
        assembler.mov(AsmJit::rax, reinterpret_cast<DWORD_PTR>(arg));
        assembler.push(AsmJit::rax);
      });
    }

    // Allocate ghost space
    assembler.sub(AsmJit::rsp, AsmJit::Imm(0x20));

    // Call target
    assembler.mov(AsmJit::rax, reinterpret_cast<DWORD_PTR>(address));
    assembler.call(AsmJit::rax);
    
    // Cleanup ghost space
    assembler.add(AsmJit::rsp, AsmJit::Imm(0x20));

    // Clean up remaining stack space
    assembler.add(AsmJit::rsp, 0x8 * (num_args - 4));

    // Write return value to memory
    assembler.mov(AsmJit::rcx, reinterpret_cast<DWORD_PTR>(
      return_value_remote.GetBase()) + i * sizeof(DWORD_PTR));
    assembler.mov(AsmJit::qword_ptr(AsmJit::rcx), AsmJit::rax);

    // Write 64-bit return value to memory
    assembler.mov(AsmJit::rcx, reinterpret_cast<DWORD_PTR>(
      return_value_64_remote.GetBase()) + i * sizeof(DWORD64));
    assembler.mov(AsmJit::qword_ptr(AsmJit::rcx), AsmJit::rax);

    // Call kernel32.dll!GetLastError
    assembler.mov(AsmJit::rax, get_last_error);
    assembler.call(AsmJit::rax);
    
    // Write error code to memory
    assembler.mov(AsmJit::rcx, reinterpret_cast<DWORD_PTR>(
      last_error_remote.GetBase()) + i * sizeof(DWORD));
    assembler.mov(AsmJit::dword_ptr(AsmJit::rcx), AsmJit::rax);

    // Epilogue
    assembler.mov(AsmJit::rsp, AsmJit::rbp);
    assembler.pop(AsmJit::rbp);
  }

  // Return
  assembler.ret();
#elif defined(_M_IX86)
  for (std::size_t i = 0; i < addresses.size(); ++i)
  {
    LPCVOID address = addresses[i];
    CallConv call_conv = call_convs[i];
    std::vector<PVOID> const& args = args_full[i];
    std::size_t const num_args = args.size();
    
    // Prologue
    assembler.push(AsmJit::ebp);
    assembler.mov(AsmJit::ebp, AsmJit::esp);

    // Call kernel32.dll!SetLastError
    assembler.push(AsmJit::Imm(0x0));
    assembler.mov(AsmJit::eax, set_last_error);
    assembler.call(AsmJit::eax);

    // Get stack arguments offset
    std::size_t stack_arg_offs = 0;
    switch (call_conv)
    {
    case CallConv::kThisCall:
      stack_arg_offs = 1;
      break;

    case CallConv::kFastCall:
      stack_arg_offs = 2;
      break;

    case CallConv::kCdecl:
    case CallConv::kStdCall:
    case CallConv::kDefault:
      stack_arg_offs = 0;
      break;

    default:
      BOOST_THROW_EXCEPTION(HadesMemError() << 
        ErrorString("Invalid calling convention."));
    }

    // Pass first arg in through ECX if 'thiscall' is specified
    if (call_conv == CallConv::kThisCall)
    {
      assembler.mov(AsmJit::ecx, num_args ? reinterpret_cast<DWORD_PTR>(
        args[0]) : 0);
    }

    // Pass first two args in through ECX and EDX if 'fastcall' is specified
    if (call_conv == CallConv::kFastCall)
    {
      assembler.mov(AsmJit::ecx, num_args ? reinterpret_cast<DWORD_PTR>(
        args[0]) : 0);
      assembler.mov(AsmJit::edx, num_args > 1 ? reinterpret_cast<DWORD_PTR>(
        args[1]) : 0);
    }

    // Pass all remaining args on stack if there are any left to process.
    if (num_args > stack_arg_offs)
    {
      std::for_each(args.crbegin(), args.crend() - stack_arg_offs, 
        [&] (PVOID arg)
      {
        assembler.mov(AsmJit::eax, reinterpret_cast<DWORD_PTR>(arg));
        assembler.push(AsmJit::eax);
      });
    }
    
    // Call target
    assembler.mov(AsmJit::eax, reinterpret_cast<DWORD_PTR>(address));
    assembler.call(AsmJit::eax);
    
    // Write return value to memory
    assembler.mov(AsmJit::ecx, reinterpret_cast<DWORD_PTR>(
      return_value_remote.GetBase()) + i * sizeof(DWORD_PTR));
    assembler.mov(AsmJit::dword_ptr(AsmJit::ecx), AsmJit::eax);
    
    // Write 64-bit return value to memory
    assembler.mov(AsmJit::ecx, reinterpret_cast<DWORD_PTR>(
      return_value_64_remote.GetBase()) + i * sizeof(DWORD64));
    assembler.mov(AsmJit::dword_ptr(AsmJit::ecx), AsmJit::eax);
    assembler.mov(AsmJit::dword_ptr(AsmJit::ecx, 4), AsmJit::edx);
    
    // Call kernel32.dll!GetLastError
    assembler.mov(AsmJit::eax, get_last_error);
    assembler.call(AsmJit::eax);
    
    // Write error code to memory
    assembler.mov(AsmJit::ecx, reinterpret_cast<DWORD_PTR>(
      last_error_remote.GetBase()) + i * sizeof(DWORD));
    assembler.mov(AsmJit::dword_ptr(AsmJit::ecx), AsmJit::eax);
    
    // Clean up stack if necessary
    if (call_conv == CallConv::kCdecl)
    {
      assembler.add(AsmJit::esp, AsmJit::Imm(num_args * sizeof(PVOID)));
    }

    // Epilogue
    assembler.mov(AsmJit::esp, AsmJit::ebp);
    assembler.pop(AsmJit::ebp);
  }

  // Return
  assembler.ret(AsmJit::Imm(0x4));
#else
#error "[HadesMem] Unsupported architecture."
#endif
  
  DWORD_PTR const stub_size = assembler.getCodeSize();
  
  Allocator const stub_mem_remote(process, stub_size);
  PBYTE const stub_remote = static_cast<PBYTE>(stub_mem_remote.GetBase());
  DWORD_PTR const stub_remote_temp = reinterpret_cast<DWORD_PTR>(stub_remote);
  
  std::vector<BYTE> code_real(stub_size);
  assembler.relocCode(code_real.data(), reinterpret_cast<DWORD_PTR>(
    stub_remote));
  
  WriteVector(process, stub_remote, code_real);
  
  HANDLE const thread_remote = CreateRemoteThread(process.GetHandle(), 
    nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(stub_remote_temp), 
    nullptr, 0, nullptr);
  if (!thread_remote)
  {
    DWORD const last_error = GetLastError();
    BOOST_THROW_EXCEPTION(HadesMemError() << 
      ErrorString("Could not create remote thread.") << 
      ErrorCodeWinLast(last_error));
  }
  
  BOOST_SCOPE_EXIT_ALL(&)
  {
    // WARNING: Handle is leaked if FreeLibrary fails.
    BOOST_VERIFY(::CloseHandle(thread_remote));
  };
  
  if (WaitForSingleObject(thread_remote, INFINITE) != WAIT_OBJECT_0)
  {
    DWORD const LastError = GetLastError();
    BOOST_THROW_EXCEPTION(HadesMemError() << 
      ErrorString("Could not wait for remote thread.") << 
      ErrorCodeWinLast(LastError));
  }
  
  std::vector<RemoteFunctionRet> return_vals;
  for (std::size_t i = 0; i < addresses.size(); ++i)
  {
    DWORD_PTR const ret_val = Read<DWORD_PTR>(process, static_cast<DWORD_PTR*>(
      return_value_remote.GetBase()) + i);
    DWORD64 const ret_val_64 = Read<DWORD64>(process, static_cast<DWORD64*>(
      return_value_64_remote.GetBase()) + i);
    DWORD const error_code = Read<DWORD>(process, static_cast<DWORD*>(
      last_error_remote.GetBase()) + i);
    return_vals.push_back(RemoteFunctionRet(ret_val, ret_val_64, error_code));
  }
  
  return return_vals;
}

}