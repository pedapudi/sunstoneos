# Give the board a Wi-Fi network's credentials

The user stores a network's SSID and password with `tools/wifi-credential.py`, in their own terminal. A board run
that names the network receives the credential from the runner. The password reaches no agent, no repository file,
no build-cache file and no board-queue record.

## Store a network

```sh
tools/wifi-credential.py add home       # prompts for the SSID, the password twice, and whether to keep the password
tools/wifi-credential.py list           # names and SSIDs only
tools/wifi-credential.py fingerprint home
tools/wifi-credential.py remove home
tools/wifi-credential.py selftest       # PBKDF2 against the IEEE 802.11i test vectors
```

- `add` reads the SSID with `input` and the password with `getpass`. It never echoes or prints either, and it
  refuses to run when its input or output is not a terminal, so an agent's script cannot drive it.
- It derives the WPA2 pairwise master key (PMK): PBKDF2-HMAC-SHA1, 4096 rounds, the SSID as salt, 32 bytes. Every run
  of `add` first checks that derivation against the three IEEE 802.11i test vectors.
- The answer to "Store the password for WPA3?" decides whether the password is kept. WPA2 needs only the PMK. WPA3
  SAE needs the password, because it derives its own key from it.
- The credential lives in `~/.config/sunstoneos/wifi/NAME.json`. The directory has mode 0700 and the file mode 0600;
  the tool writes the file atomically. The file holds the SSID, the PMK and, on request, the password.
- The fingerprint is the first 8 bytes of HMAC-SHA256 of the PMK under the label
  `sunstoneos wifi credential fingerprint v1`. It identifies the credential without revealing it.
- A name is 1 to 40 characters of letters, digits, `_`, `.` and `-`. A WPA2 passphrase is 8 to 63 printable ASCII
  characters.

## Use a network in a run

An agent submits with `--wifi NAME` ([board runner](board-runner.md#deliver-a-wi-fi-credential)). The job records the
name and the fingerprint. The daemon reads the file when the run starts, so changing the password with `add` after a
submission refuses that run; the submitter then resubmits.

## What the OS does with it

- The runner uploads one 256-byte record as its own input to `0x43004000`, in the radio profile span, from memory.
  The span is EL1 read-write with no EL0 access, so no application reads it.
- The radio's supplicant calls `ss_wifi_credential` (`include/sunstone_wifi_credential.h`) at EL1. It receives pointers
  to the SSID, the 32-byte PMK and the password (NULL when only the PMK was stored). A session without an upload gets
  `-ENOENT`.
- The OS wipes the record with stores the compiler cannot remove, and cleans the cache lines, when the session ends
  (`ss_finish`) and when the board fails (`board_fail`). Later calls return `-ENOENT`.
- The value appears in no result word, failure note or diagnostic.

## What the runner does when the OS cannot wipe

A run that ends in a watchdog reset or a hang never reaches `board_fail` or the radio owner's wipe, so the runner zeroes the
record itself. On every exit of a session that began the upload (a normal return, a failure return, a host error, a
watchdog return to FEL), while it still holds the board lock, it reads the region back. If the OS wiped it, the region reads
zero and the runner writes nothing. Otherwise it writes 256 zero bytes over `0x43004000` by FEL and reads them back as zero.
After a watchdog return the fresh instance's DRAM is brought up again first, as at the start of any run. The result records
`credential_wipe`: the address, the size, `wiped-by-session` or `zeroed-by-runner`, and that the readback was zero. No
credential byte is read into a file or a log: the readback goes to an anonymous memory file.

If the board does not return to FEL, or the zero write or its readback fails, the entry records `credential_may_remain`. A
marker file for the port, `~/.cache/sunstoneos/board-queue/credential-residue/PORT.json`, is written before the upload and
removed only after a zero readback; it holds the port, the address and the size. The next run on the port zeroes the region
before anything else, and a credential run is refused (`refused-before-launch`) until that zero write succeeds.

## What is checked

`checks/wifi-credential.py` uses a fake credential in a temporary directory, never `~/.config`. It checks the PMK
against the test vectors, the prompts and file modes under a pseudo-terminal, the refusals (missing file, changed
fingerprint, bad file mode, bad directory mode, link), a scan of every queue file for the password and the PMK in raw,
hex and base64 form after a dry-run launch, the scan's failure when the PMK is written into the entry, and the OS
receiver on the host and as an AArch64 object. `checks/credential-zero.py` models the board's memory and plans the zero write for
each exit kind (session wiped, normal return, failure return, host error, watchdog return, no return, failed transport, a
zero write that does not read back), scans every file for the credential, and fails when the final step, the zero write, the
DRAM bring-up, the marker or the start-of-run clearing is removed. No board run has exercised the receiver.
