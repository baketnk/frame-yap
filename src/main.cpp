#include <iostream>
#include <string_view>

namespace {
void help() {
    std::cout << "Frame Dictation — standalone Steam Frame voice typing\n"
                 "Scaffold only: overlay, microphone, ASR and input are not implemented.\n\n"
                 "Usage: frame-dictation [--help | --version]\n"
                 "No device access or background processes are started.\n";
}
}

int main(int argc, char** argv) {
    if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) {
        help();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "frame-dictation " << FRAME_DICTATION_VERSION << " (scaffold)\n";
        return 0;
    }
    std::cerr << "Unsupported arguments. Use --help; runtime features are not implemented.\n";
    return 2;
}
