from conan import ConanFile
from conan.tools.meson import MesonToolchain, Meson
from conan.tools.gnu import PkgConfigDeps
from conan.tools.layout import basic_layout


class NadekoConan(ConanFile):
    name = "nadeko"
    version = "0.1"
    settings = "os", "compiler", "build_type", "arch"
    generators = "PkgConfigDeps", "MesonToolchain"
    exports_sources = "meson.build", "*.c"
    requires = "sqlite3/3.39.4", "libarchive/3.6.1"

    def layout(self):
        basic_layout(self)

    def build(self):
        meson = Meson(self)
        meson.configure()
        meson.build()

    def package(self):
        meson = Meson(self)
        meson.install()

    def package_info(self):
        self.cpp_info.libs = ["nadeko", "lines"]

