from conans import ConanFile, CMake, tools


class UbitrackCoreConan(ConanFile):
    name = "ubitrack_device_comm_zmq"
    version = "1.3.0"

    description = "Ubitrack Device Communication ZMQ"
    url = "https://github.com/Ubitrack/device_comm_zmq.git"
    license = "GPL"

    short_paths = True
    settings = "os", "compiler", "build_type", "arch"
    options = {
        "workspaceBuild": [True, False],
    }
    generators = "cmake"

    default_options = {
        "ubitrack_core:shared" : True,
        "ubitrack_vision:shared" : True,
        "ubitrack_dataflow:shared" : True,
        "zmq:shared" : True,
        "workspaceBuild" : False,
        }

    # all sources are deployed with the package
    exports_sources = "doc/*", "src/*", "CMakeLists.txt"

    def requirements(self):

        userChannel = "ubitrack/stable"
        if self.options.workspaceBuild:
            userChannel = "local/dev"

        self.requires("ubitrack_core/%s@%s" % (self.version, userChannel))
        self.requires("ubitrack_vision/%s@%s" % (self.version, userChannel))
        self.requires("ubitrack_dataflow/%s@%s" % (self.version, userChannel))

        self.requires("zmq/[>=4.3.2]@camposs/stable")
        self.requires("azmq/1.0.3@camposs/stable")
        self.requires("msgpack/[>=3.2.0]@camposs/stable")

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
        cmake.install()

    def package(self):
        pass

    def package_info(self):
        pass

    def package_id(self):
        self.info.requires["ubitrack_vision"].full_package_mode()
