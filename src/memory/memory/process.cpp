// Copyright (C) 2010-2013 Joshua Boyce.
// See the file COPYING for copying permission.

#include <hadesmem/process.hpp>

#include <vector>
#include <ostream>
#include <utility>

#include <hadesmem/error.hpp>
#include <hadesmem/config.hpp>
#include <hadesmem/detail/assert.hpp>

namespace hadesmem
{

namespace
{

bool IsWoW64Process(HANDLE handle)
{
  BOOL is_wow64 = FALSE;
  if (!::IsWow64Process(handle, &is_wow64))
  {
    DWORD const last_error = ::GetLastError();
    HADESMEM_THROW_EXCEPTION(Error() << 
      ErrorString("IsWoW64Process failed.") << 
      ErrorCodeWinLast(last_error));
  }

  return is_wow64 != FALSE;
}

HANDLE OpenProcessAllAccess(DWORD id)
{
  HANDLE const handle = ::OpenProcess(PROCESS_ALL_ACCESS, TRUE, id);
  if (!handle)
  {
    DWORD const last_error = ::GetLastError();
    HADESMEM_THROW_EXCEPTION(Error() << 
      ErrorString("OpenProcess failed.") << 
      ErrorCodeWinLast(last_error));
  }

  return handle;
}

HANDLE DuplicateHandle(HANDLE handle)
{
  HADESMEM_ASSERT(handle != nullptr);

  HANDLE new_handle = nullptr;
  if (!::DuplicateHandle(::GetCurrentProcess(), handle, 
    ::GetCurrentProcess(), &new_handle, 0, TRUE, DUPLICATE_SAME_ACCESS))
  {
    DWORD const last_error = ::GetLastError();
    HADESMEM_THROW_EXCEPTION(Error() << 
      ErrorString("DuplicateHandle failed.") << 
      ErrorCodeWinLast(last_error));
  }

  return new_handle;
}

}

Process::Process(DWORD id)
  : handle_(OpenProcessAllAccess(id)), 
  id_(id)
{
  CheckWoW64();
}

Process::Process(Process const& other)
  : handle_(DuplicateHandle(other.handle_.GetHandle())), 
  id_(other.id_)
{ }

Process& Process::operator=(Process const& other)
{
  handle_ = DuplicateHandle(other.handle_.GetHandle());
  id_ = other.id_;
  
  return *this;
}

Process::Process(Process&& other) HADESMEM_NOEXCEPT
  : handle_(std::move(other.handle_)), 
  id_(other.id_)
{
  other.id_ = 0;
}

Process& Process::operator=(Process&& other) HADESMEM_NOEXCEPT
{
  handle_ = std::move(other.handle_);
  id_ = other.id_;

  other.id_ = 0;
  
  return *this;
}

Process::~Process() HADESMEM_NOEXCEPT
{
  CleanupUnchecked();
}

DWORD Process::GetId() const HADESMEM_NOEXCEPT
{
  return id_;
}

HANDLE Process::GetHandle() const HADESMEM_NOEXCEPT
{
  return handle_.GetHandle();
}

void Process::Cleanup()
{
  handle_.Cleanup();
  id_ = 0;
}

void Process::CheckWoW64() const
{
  if (IsWoW64Process(::GetCurrentProcess()) != 
    IsWoW64Process(handle_.GetHandle()))
  {
    HADESMEM_THROW_EXCEPTION(Error() << 
      ErrorString("Cross-architecture process manipulation is currently "
      "unsupported."));
  }
}

void Process::CleanupUnchecked() HADESMEM_NOEXCEPT
{
  try
  {
    Cleanup();
  }
  catch (std::exception const& e)
  {
    (void)e;

    // WARNING: Handle is leaked if 'Cleanup' fails.
    HADESMEM_ASSERT(boost::diagnostic_information(e).c_str() && false);

    id_ = 0;
    handle_ = nullptr;
  }
}

bool operator==(Process const& lhs, Process const& rhs) HADESMEM_NOEXCEPT
{
  return lhs.GetId() == rhs.GetId();
}

bool operator!=(Process const& lhs, Process const& rhs) HADESMEM_NOEXCEPT
{
  return !(lhs == rhs);
}

bool operator<(Process const& lhs, Process const& rhs) HADESMEM_NOEXCEPT
{
  return lhs.GetId() < rhs.GetId();
}

bool operator<=(Process const& lhs, Process const& rhs) HADESMEM_NOEXCEPT
{
  return lhs.GetId() <= rhs.GetId();
}

bool operator>(Process const& lhs, Process const& rhs) HADESMEM_NOEXCEPT
{
  return lhs.GetId() > rhs.GetId();
}

bool operator>=(Process const& lhs, Process const& rhs) HADESMEM_NOEXCEPT
{
  return lhs.GetId() >= rhs.GetId();
}

std::ostream& operator<<(std::ostream& lhs, Process const& rhs)
{
  return (lhs << rhs.GetId());
}

std::wostream& operator<<(std::wostream& lhs, Process const& rhs)
{
  return (lhs << rhs.GetId());
}

std::wstring GetPath(Process const& process)
{
  std::vector<wchar_t> path(HADESMEM_MAX_PATH_UNICODE);
  DWORD path_len = static_cast<DWORD>(path.size());
  if (!::QueryFullProcessImageName(process.GetHandle(), 0, path.data(), 
    &path_len))
  {
      DWORD const last_error = ::GetLastError();
      HADESMEM_THROW_EXCEPTION(Error() << 
        ErrorString("QueryFullProcessImageName failed.") << 
        ErrorCodeWinLast(last_error));
  }
  
  return path.data();
}

bool IsWoW64(Process const& process)
{
  return IsWoW64Process(process.GetHandle());
}

void GetSeDebugPrivilege()
{
  HANDLE process_token_temp = 0;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | 
    TOKEN_QUERY, &process_token_temp)) 
  {
    DWORD const last_error = ::GetLastError();
    HADESMEM_THROW_EXCEPTION(Error() << 
      ErrorString("OpenProcessToken failed.") << 
      ErrorCodeWinLast(last_error));
  }
  detail::SmartHandle const process_token(process_token_temp);

  LUID luid = { 0, 0 };
  if (!::LookupPrivilegeValue(nullptr, SE_DEBUG_NAME, &luid))
  {
    DWORD const last_error = ::GetLastError();
    HADESMEM_THROW_EXCEPTION(Error() << 
      ErrorString("LookupPrivilegeValue failed.") << 
      ErrorCodeWinLast(last_error));
  }

  TOKEN_PRIVILEGES privileges;
  ::ZeroMemory(&privileges, sizeof(privileges));
  privileges.PrivilegeCount = 1;
  privileges.Privileges[0].Luid = luid;
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (!::AdjustTokenPrivileges(process_token.GetHandle(), FALSE, &privileges, 
    sizeof(privileges), nullptr, nullptr) || 
    ::GetLastError() == ERROR_NOT_ALL_ASSIGNED) 
  {
    DWORD const last_error = ::GetLastError();
    HADESMEM_THROW_EXCEPTION(Error() << 
      ErrorString("AdjustTokenPrivileges failed.") << 
      ErrorCodeWinLast(last_error));
  }
}

}
