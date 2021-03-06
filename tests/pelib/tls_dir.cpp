// Copyright (C) 2010-2015 Joshua Boyce
// See the file COPYING for copying permission.

#include <hadesmem/pelib/tls_dir.hpp>
#include <hadesmem/pelib/tls_dir.hpp>

#include <sstream>
#include <utility>

#include <hadesmem/detail/warning_disable_prefix.hpp>
#include <boost/detail/lightweight_test.hpp>
#include <hadesmem/detail/warning_disable_suffix.hpp>

#include <hadesmem/config.hpp>
#include <hadesmem/error.hpp>
#include <hadesmem/module.hpp>
#include <hadesmem/module_list.hpp>
#include <hadesmem/pelib/pe_file.hpp>
#include <hadesmem/process.hpp>
#include <hadesmem/read.hpp>

// TODO: Ensure that base address is different across modules (similar to other
// tests)

void TestTlsDir()
{
  // Use TLS to ensure that at least one module has a TLS dir
  thread_local static std::int32_t tls_dummy = 0;

  hadesmem::Process const process(::GetCurrentProcessId());

  hadesmem::PeFile pe_file_1(
    process, ::GetModuleHandleW(nullptr), hadesmem::PeFileType::Image, 0);

  hadesmem::TlsDir tls_dir_1(process, pe_file_1);

  hadesmem::TlsDir tls_dir_2(tls_dir_1);
  BOOST_TEST_EQ(tls_dir_1, tls_dir_2);
  tls_dir_1 = tls_dir_2;
  BOOST_TEST_EQ(tls_dir_1, tls_dir_2);
  hadesmem::TlsDir tls_dir_3(std::move(tls_dir_2));
  BOOST_TEST_EQ(tls_dir_3, tls_dir_1);
  tls_dir_2 = std::move(tls_dir_3);
  BOOST_TEST_EQ(tls_dir_1, tls_dir_2);

  hadesmem::ModuleList modules(process);
  for (auto const& mod : modules)
  {
    hadesmem::PeFile const cur_pe_file(
      process, mod.GetHandle(), hadesmem::PeFileType::Image, 0);

    std::unique_ptr<hadesmem::TlsDir> cur_tls_dir;
    try
    {
      cur_tls_dir = std::make_unique<hadesmem::TlsDir>(process, cur_pe_file);
    }
    catch (std::exception const& /*e*/)
    {
      continue;
    }

    ++tls_dummy;

    auto const tls_dir_raw =
      hadesmem::Read<IMAGE_TLS_DIRECTORY>(process, cur_tls_dir->GetBase());

    cur_tls_dir->SetStartAddressOfRawData(
      cur_tls_dir->GetStartAddressOfRawData());
    cur_tls_dir->SetEndAddressOfRawData(cur_tls_dir->GetEndAddressOfRawData());
    cur_tls_dir->SetAddressOfIndex(cur_tls_dir->GetAddressOfIndex());
    cur_tls_dir->SetAddressOfCallBacks(cur_tls_dir->GetAddressOfCallBacks());
    cur_tls_dir->SetSizeOfZeroFill(cur_tls_dir->GetSizeOfZeroFill());
    cur_tls_dir->SetCharacteristics(cur_tls_dir->GetCharacteristics());
    std::vector<PIMAGE_TLS_CALLBACK> callbacks;
    cur_tls_dir->GetCallbacks(std::back_inserter(callbacks));
    cur_tls_dir->UpdateWrite();
    cur_tls_dir->UpdateRead();

    auto const tls_dir_raw_new =
      hadesmem::Read<IMAGE_TLS_DIRECTORY>(process, cur_tls_dir->GetBase());

    BOOST_TEST_EQ(
      std::memcmp(&tls_dir_raw, &tls_dir_raw_new, sizeof(tls_dir_raw)), 0);

    std::stringstream test_str_1;
    test_str_1.imbue(std::locale::classic());
    test_str_1 << *cur_tls_dir;
    std::stringstream test_str_2;
    test_str_2.imbue(std::locale::classic());
    test_str_2 << cur_tls_dir->GetBase();
    BOOST_TEST_EQ(test_str_1.str(), test_str_2.str());
  }

  BOOST_TEST_NE(tls_dummy, 0);
}

int main()
{
  TestTlsDir();
  return boost::report_errors();
}
