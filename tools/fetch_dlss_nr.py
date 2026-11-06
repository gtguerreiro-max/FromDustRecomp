"""Fetch NVIDIA DLSS 5 Neural Rendering (NGX feature 18) into third_party/dlss_nr.

The files come from the RTX Remix external package rtx-remix-ngx_sdk_dlnr@2,
hosted on NVIDIA's packman remote that the dxvk-remix build uses
(scripts-common/packman/external_config.packman.xml). The package carries the
NGX headers, nvngx_dlssnr.dll and NVIDIA's RTX SDKs licence (LICENSE.txt):
read it before redistributing the DLL.

The DLL must carry a valid Authenticode signature from NVIDIA Corporation and
the expected SHA-256, or nothing is installed. Needs py7zr (pip install py7zr).

Usage: python tools/fetch_dlss_nr.py
"""
import hashlib
import io
import pathlib
import shutil
import subprocess
import sys
import tempfile
import urllib.request

URL = 'https://d1oqksizpb0e3d.cloudfront.net/rtx-remix-ngx_sdk_dlnr@2.7z'
DLL = 'lib/Windows_x86_64/rel/nvngx_dlssnr.dll'
DLL_SHA256 = '2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f'
ROOT = pathlib.Path(__file__).resolve().parent.parent
TARGET = ROOT / 'third_party' / 'dlss_nr'


def signer(path):
    script = ("$s=Get-AuthenticodeSignature -LiteralPath '%s';"
              "\"$($s.Status)|$($s.SignerCertificate.Subject)\"" % str(path).replace("'", "''"))
    out = subprocess.run(['powershell', '-NoProfile', '-Command', script], capture_output=True, text=True, check=True)
    status, _, subject = out.stdout.strip().partition('|')
    return status, subject


def main():
    try:
        import py7zr
    except ImportError:
        sys.exit('py7zr is needed: pip install py7zr')
    print('downloading', URL)
    data = urllib.request.urlopen(URL, timeout=120).read()
    with tempfile.TemporaryDirectory() as tmp:
        work = pathlib.Path(tmp) / 'dlnr'
        with py7zr.SevenZipFile(io.BytesIO(data)) as archive:
            archive.extractall(work)
        dll = work / DLL
        digest = hashlib.sha256(dll.read_bytes()).hexdigest()
        status, subject = signer(dll)
        print('nvngx_dlssnr.dll sha256', digest)
        print('signature', status, subject)
        if digest != DLL_SHA256:
            sys.exit('unexpected nvngx_dlssnr.dll (sha256 differs): not installed')
        if status != 'Valid' or 'O=NVIDIA Corporation' not in subject:
            sys.exit('nvngx_dlssnr.dll is not signed by NVIDIA: not installed')
        if TARGET.exists():
            shutil.rmtree(TARGET)
        TARGET.mkdir(parents=True)
        for name in ('include', 'lib', 'LICENSE.txt'):
            source = work / name
            (shutil.copytree if source.is_dir() else shutil.copy2)(source, TARGET / name)
    print('installed in', TARGET, '- re-run cmake so FD_DLSS_NR is defined')


if __name__ == '__main__':
    main()
