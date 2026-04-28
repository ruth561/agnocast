#include <glog/logging.h>

#include <csignal>
#include <iostream>

namespace
{

void cause_sigsegv()
{
  (void)std::raise(SIGSEGV);
}

}  // namespace

int main(int argc, char ** argv)
{
  (void)argc;

  google::InitGoogleLogging(argv[0]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  google::InstallFailureSignalHandler();

  std::cout << "Hello, glog!\n";
  cause_sigsegv();

  return 0;
}
