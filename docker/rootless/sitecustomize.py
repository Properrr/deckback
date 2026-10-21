# Loaded via PYTHONPATH only under rootless Docker (scripts/lib.sh: compose_run), where we run as a
# container root that cannot chown to uids outside its user namespace. Python's tarfile treats that
# chown failure as non-fatal and then silently SKIPS the member's chmod, so extracted executables
# lose their exec bit (e.g. Chromium's third_party/node). Ownership is meaningless in this setup.
import tarfile

tarfile.TarFile.chown = lambda self, *args, **kwargs: None
