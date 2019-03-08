{
    "targets": [{
        "target_name": "binding",
        "sources": [
            "binding.cc",
        ],
        "include_dirs": [
            "<!(node -e \"require('nan')\")",
            "<!@(pkg-config --cflags-only-I libjpeg)",
        ],
        "cflags_cc": [
            "-Wstrict-aliasing",
            "-Wextra",
            "-march=native",
            "-mtune=generic"
        ],
        "libraries": [
            '<!@(pkg-config --libs libjpeg)'
        ],
    }]
}
