# Authorized SSH keys for this board

Every `*.pub` file here is concatenated into `/root/.ssh/authorized_keys` in the
image by `tools/stage_board_extras.sh`.

This exists because the board's root password is empty
(`BR2_TARGET_GENERIC_ROOT_PASSWD=""` in
`buildroot/configs/milkv-duos-glibc-arm64-sd_defconfig`), and dropbear
unconditionally refuses blank-password logins -- see
`src/svr-authpasswd.c:101` in dropbear 2024.86, which has no build option or
runtime flag to relax it. Key auth is therefore the only way in over the
network; the serial console still logs in with no password.

## Adding another computer

On that machine, print its public key (create one with `ssh-keygen -t ed25519`
if it has none):

    cat ~/.ssh/id_ed25519.pub

Save it here under a name that identifies the machine, then:

    tools/stage_board_extras.sh milkv-duos-glibc-arm64-sd
    ./build.sh milkv-duos-glibc-arm64-sd        # reflash required

To add a key to a board that is already running, without reflashing, just
append it over an existing session:

    ssh root@192.168.42.1 'cat >> /root/.ssh/authorized_keys' < ~/.ssh/id_ed25519.pub

## A note on committing these

Public keys are not secret, but they do identify which machines you use, and
this repo has a public GitHub remote. `.gitignore` here excludes `*.pub` by
default for that reason. If you would rather have the key set reproducible from
a fresh clone, delete that line -- there is no security problem with doing so.
