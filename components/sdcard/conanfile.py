from conan import ConanFile


class EpdSdcardRecipe(ConanFile):
    name = "epd-sdcard"
    version = "1.0.0"
    license = "LicenseRef-SiFli"
    description = "SD card media lifecycle service for SiFli SDK and RT-Thread DFS."
    topics = ("sdcard", "storage", "hotplug", "rt-thread")

    support_sdk_version = "2.5.0"
    python_requires = "sf-pkg-base/[^1.0]@sifli"
    python_requires_extend = "sf-pkg-base.SourceOnlyBase"

    exports_sources = (
        "conandata.yml",
        "Kconfig",
        "SConscript",
        "LICENSE",
        "include/*",
        "src/*",
        "port/*",
        "example/*",
    )

