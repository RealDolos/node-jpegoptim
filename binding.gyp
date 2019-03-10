{
    "variables": {
        "exif": "<!(pkg-config --exists libexif && echo yes || echo no)",
    },
    "targets": [{
        "target_name": "binding",
        "sources": [
            "binding.cc",
        ],
        "include_dirs": [
            "<!(node -e \"require('nan')\")",
            "<!@(pkg-config --cflags-only-I libjpeg | sed s/-I//g)",
        ],
        "cflags_cc": [
            "-Wstrict-aliasing",
            "-Wextra",
            "-march=native",
            "-mtune=generic"
        ],
        "libraries": [
            '<!@(pkg-config --libs libjpeg)',
        ],
        "conditions": [
            ['exif=="yes"', {
                "include_dirs": [
                    "<!@(pkg-config --cflags-only-I libexif | sed s/-I//g)",
                ],
                "cflags_cc": [
                    "-DHAS_EXIF=1",
                ],
                "libraries": [
                    '<!@(pkg-config --libs libexif)',
                ],
            }]
        ]
    }]
}
