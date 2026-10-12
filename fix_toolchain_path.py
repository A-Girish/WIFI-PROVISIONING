Import("env")
import os

# pioarduino's toolchain zip extracts to a nested folder; make sure its bin dir is on PATH
pkg = os.path.join(os.path.expanduser("~"), ".platformio", "packages", "toolchain-xtensa-esp-elf")
for sub in (os.path.join("xtensa-esp-elf", "bin"), "bin"):
    d = os.path.join(pkg, sub)
    if os.path.isdir(d):
        env.PrependENVPath("PATH", d)
        os.environ["PATH"] = d + os.pathsep + os.environ["PATH"]
