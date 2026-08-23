#include "core/apply_patch.h"

#include <cassert>
#include <filesystem>
#include <fstream>

int main() {
  const auto dir =
      std::filesystem::temp_directory_path() / "pici-apply-patch-test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  {
    std::ofstream(dir / "file.txt") << "one\ntwo\n";
    pi::core::ParseDiagnostic error;
    const std::string input =
        "*** Begin Patch" + std::string(1, '\n') +
        "*** Update File: file.txt\n@@\n-one\n+ONE\n*** End Patch\n";
    const auto patch = pi::core::parse_patch(input, true, error);
    assert(patch);
    pi::core::apply_patch(dir, *patch);
  }
  std::ifstream in(dir / "file.txt");
  const std::string content{std::istreambuf_iterator<char>(in), {}};
  assert(content == "ONE\ntwo\n");
  std::filesystem::remove_all(dir);
}
