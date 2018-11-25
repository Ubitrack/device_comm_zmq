from conans import ConanFile, CMake, tools


class UbitrackCoreConan(ConanFile):
    name = "ubitrack_device_comm_zmq"
    version = "1.3.0"

    description = "Ubitrack Device Communication ZMQ"
    url = "https://github.com/Ubitrack/device_comm_zmq.git"
    license = "GPL"

    short_paths = True
    settings = "os", "compiler", "build_type", "arch"
    options = {"with_msgpack": [True, False]}
    generators = "cmake"

    requires = (
        "zmq/[>=4.2.2]@camposs/stable",
        "cppzmq/[>=4.2.2]@camposs/stable",

        "ubitrack_core/%s@ubitrack/stable" % version,
        "ubitrack_vision/%s@ubitrack/stable" % version,
        "ubitrack_dataflow/%s@ubitrack/stable" % version,
       )

    default_options = (
        "ubitrack_core:shared=True",
        "ubitrack_vision:shared=True",
        "ubitrack_dataflow:shared=True",
        "zmq:shared=True",
        "with_msgpack=True",
        )

    # all sources are deployed with the package
    exports_sources = "doc/*", "src/*", "CMakeLists.txt"

    def requirements(self):
        if self.options.with_msgpack:
            self.requires("msgpack/[>=2.1.5]@camposs/stable")

    def imports(self):
        self.copy(pattern="*.dll", dst="bin", src="bin") # From bin to bin
        self.copy(pattern="*.dylib*", dst="lib", src="lib") 
        self.copy(pattern="*.so*", dst="lib", src="lib") 
       
    def build(self):
        cmake = CMake(self)
        cmake.definitions["WITH_MSGPACK"] = self.options.with_msgpack
        cmake.configure()
        cmake.build()
        cmake.install()

    def package(self):
        pass

    def package_info(self):
        pass

    def package_id(self):
        self.info.requires["ubitrack_vision"].full_package_mode()
