"""Read device info or send one explicitly selected file to a real PSP.
The PSP will ask for approval and a destination. Python standard library only.
"""
import argparse
import hashlib
import http.client
import ipaddress
import json
import pathlib
import urllib.parse
import uuid

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('ip', type=ipaddress.IPv4Address)
parser.add_argument('--port', type=int, default=53317)
parser.add_argument('--send', type=pathlib.Path, help='File to send after approval on the PSP')
parser.add_argument('--chunked', action='store_true', help='Test chunked file uploads')
args = parser.parse_args()
if not 1 <= args.port <= 65535:
    parser.error('Port must be between 1 and 65535')

def connection():
    return http.client.HTTPConnection(str(args.ip), args.port, timeout=150)

conn = connection()
conn.request('GET', '/api/localsend/v2/info')
res = conn.getresponse()
body = res.read()
if res.status != 200:
    raise SystemExit(f'Info failed: HTTP {res.status} {body!r}')
print(json.dumps(json.loads(body), indent=2))
conn.close()
if args.send:
    path = args.send.resolve(strict=True)
    if not path.is_file() or path.stat().st_size > 2147483647:
        parser.error('Select a regular file under 2 GiB')
    with path.open('rb') as f:
        digest = hashlib.file_digest(f, 'sha256').hexdigest()
    file_id = str(uuid.uuid4())
    metadata = {'info': {'alias': 'PocketSend hardware test', 'version': '2.1',
                        'deviceType': 'desktop', 'deviceModel': 'Python',
                        'fingerprint': str(uuid.uuid4()), 'protocol': 'http', 'port': args.port},
                'files': {file_id: {'id': file_id, 'fileName': path.name,
                                    'size': path.stat().st_size,
                                    'fileType': 'application/octet-stream', 'sha256': digest}}}
    print('Approve this file and select a folder on the PSP:', path.name)
    conn = connection()
    conn.request('POST', '/api/localsend/v2/prepare-upload',
                 json.dumps(metadata).encode(), {'Content-Type': 'application/json'})
    res = conn.getresponse()
    body = res.read()
    if res.status != 200:
        raise SystemExit(f'Preparation failed: HTTP {res.status} {body!r}')
    accepted = json.loads(body)
    conn.close()
    query = urllib.parse.urlencode({'sessionId': accepted['sessionId'], 'fileId': file_id,
                                    'token': accepted['files'][file_id]})
    conn = connection()
    with path.open('rb') as f:
        headers = {'Content-Type': 'application/octet-stream'}
        if not args.chunked:
            headers['Content-Length'] = str(path.stat().st_size)
        conn.request('POST', '/api/localsend/v2/upload?'+query,
                     iter(lambda: f.read(32768), b''), headers, encode_chunked=args.chunked)
        res = conn.getresponse()
        body = res.read()
    conn.close()
    if res.status != 200:
        raise SystemExit(f'Upload failed: HTTP {res.status} {body!r}')
    print('PSP reported a completed transfer with a matching SHA-256:', digest)
