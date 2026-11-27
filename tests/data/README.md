# Test data

Both demos are anonymized with `tfdemo anonymize` (default options): the players are `RED n`, `BLU n` and `SPEC n` with
fake accounts from `[U:1:1000000001]` on, the server is `anonymous server`, and texts hold no addresses.

## client-start.dem (in this directory)

The header and signon frames of a client recording on a local test server, and the first 100 bytes of the frame after
them, cut in the middle of that frame. It was anonymized whole and then cut:

```
tfdemo anonymize client.dem client-anonymous.dem
signon=$(python3 -c 'import struct, sys; f = open(sys.argv[1], "rb"); f.seek(1068); print(struct.unpack("<i", f.read(4))[0])' client-anonymous.dem)
head -c "$((1072 + signon + 100))" client-anonymous.dem > client-start.dem
```

## The 6v6 match (downloaded)

`tf-demo-parser-cpp-test-data-2.tar.xz`, an asset of the `test-data-2` release, holds `process-6v6.dem`: a public 6v6
match of 30 minutes on cp_process_f12 (4:3, 12 players). CMake downloads it with `-DTFDEMO_TEST_DATA=ON`, checks its
SHA-256 (`TFDEMO_TEST_DATA_SHA256` in `tests/CMakeLists.txt`) and unpacks it into
`TFDEMO_TEST_DATA_DIR/<SHA-256>/demos`; `TFDEMO_TEST_DATA_URL` takes a `file://` URL of a local copy,
`TFDEMO_TEST_DATA_DIR` keeps it between builds. It was made from the original demo (`original.dem`) with:

```
tfdemo anonymize original.dem process-6v6.dem
tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner --format=gnu -cf - process-6v6.dem \
  | xz -9e -T1 > tf-demo-parser-cpp-test-data-2.tar.xz
```

A new version of the test data is a new release (`test-data-3`) with its own SHA-256; the old one stays for the tags
that test against it.
