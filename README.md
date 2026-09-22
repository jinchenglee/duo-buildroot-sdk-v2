# Milk-V Duo series buildroot SDK V2

```
./build.sh lunch
```

For more detailed documentation, please refer to: [https://milkv.io/docs/duo/getting-started/buildroot-sdk](https://milkv.io/docs/duo/getting-started/buildroot-sdk)

## Always build in Docker

Use the container for **every** build, including the TinyTag app build below.

```sh
# once per machine
docker run --privileged -itd --name duodocker \
    -v "$(pwd)":/home/work milkvtech/milkv-duo:latest /bin/bash

# every build
docker exec -it duodocker /bin/bash -c \
    "cd /home/work && export FORCE_UNSAFE_CONFIGURE=1 && ./build.sh milkv-duos-glibc-arm64-sd"
```

**Never mix host and container builds.** The container runs as root, so every
object file, generated source and build directory it writes is owned by root
and mode 0644. A later build as your own user then dies part-way through with
errors that look like a broken toolchain but are not, e.g.:

    flex: could not create scripts/kconfig/zconf.lex.c
    bison: scripts/kconfig/zconf.tab.c: cannot open: Permission denied

That is simply a root-owned file your user cannot overwrite. There is no
partial fix -- the trees involved hold hundreds of thousands of files
(`buildroot/output` alone is ~500k). Recovering means either going back to
Docker (root can overwrite its own files, so nothing else is needed) or taking
the whole tree over once and never using Docker again:

    sudo chown -R "$(id -u):$(id -g)" .

Docker is the recommended choice: it is what Milk-V documents, it carries the
full dependency set, and `FORCE_UNSAFE_CONFIGURE=1` exists precisely because
buildroot refuses to run as root without it.

Note a failed build still runs `clean_all` first, so it wipes
`install/soc_<project>/` -- including the TPU SDK the TinyTag app links
against. After a failed build, run a full successful one before rebuilding the
app.

## Clean rebuild (keeping downloads)

`clean_all` does not touch `buildroot/output`, so it is not a from-scratch
build. To wipe every build output while keeping `buildroot/dl` (the download
cache) and `host-tools` (the toolchains):

```sh
scripts/clean_keep_dl.sh      # dry run: list what would be removed
scripts/clean_keep_dl.sh -f   # remove it
```

It deletes exactly the git-ignored files, so tracked sources and uncommitted
edits are safe. That includes `out/`, so copy any image you want to keep
first. The build outputs are root-owned, so the script runs inside the
`duodocker` container (`DUO_CONTAINER` overrides the name). Then rebuild with
the full sequence below, starting from step 1.

## TinyTag detector (this fork)

This fork adds a TinyTag AprilTag detector that runs on the Duo S NPU. It is
**not** built by `./build.sh` — it has to be cross-compiled separately and
staged into the board overlay first:

```sh
D="docker exec -it duodocker /bin/bash -c"
B=milkv-duos-glibc-arm64-sd

$D "cd /home/work && export FORCE_UNSAFE_CONFIGURE=1 && ./build.sh $B"   # 1. full build (produces the TPU SDK)
$D "cd /home/work && ./apps/tinytag_detect/build.sh $B"                  # 2. build TinyTag, stage it
$D "cd /home/work && ./apps/aruco_nano/build.sh $B"                       # 3. build ArUco Nano, stage it
$D "cd /home/work && export FORCE_UNSAFE_CONFIGURE=1 && ./build.sh $B"   # 4. rebuild so the image picks both up
```

The two app-build commands cannot be folded into step 1: they cross-compile against the cvitek TPU
SDK that step 1 itself produces, so on a first-ever build there is nothing to
link against yet.

**Re-run steps 2 and 3 after every change to the app, the model, or the board
overlay layout.** Skipping step 2 is silent — the image builds successfully and
simply has no detector in it.

New to this work? Start with [docs/handover.md](docs/handover.md).

See [apps/tinytag_detect/README.md](apps/tinytag_detect/README.md) for usage,
[tools/tinytag_cvimodel/README.md](tools/tinytag_cvimodel/README.md) for the
model conversion toolchain, and [docs/](docs/) for performance findings and the
OV9281 camera porting notes.

