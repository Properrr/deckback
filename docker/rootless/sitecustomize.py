# Loaded via PYTHONPATH under rootless Docker only. Container root cannot chown to unmapped uids, and
# tarfile then silently skips the member's chmod (executables lose their exec bit), so chown is a no-op.
import tarfile

tarfile.TarFile.chown = lambda self, *args, **kwargs: None
