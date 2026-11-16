payload\ -- vendored part of the CVE-2026-49176 PoC
==================================================

These three binaries are taken unchanged from the WalletService proof of
concept (CVE-2026-49176), because they do the parts that belong to the exploit
itself:

    wallet_ese_seed.exe        creates <dir>\Wallet\wallet.db with a persisted
                               callback that points at wallet_callback_shell.dll
    wallet_callback_shell.dll  loaded by WalletService as LocalSystem; creates a
                               demand-start service for shell_broker.exe
    documents_path.exe         reads/writes the Documents known folder
                               (FOLDERID_Documents) without Explorer

SHA256:

    17D26D6F2AE32DEF93730DD539EC406EA0977D1C10E67756E979AFCBA3443184  documents_path.exe
    EBEF3F38E7C7A929E0AFDAF913363802AFF56077452B6580C314C35ADF33DFD4  wallet_callback_shell.dll
    A636854B49451225CE294C28A0A4D4239587FF196C1D8659CAD0EA6500DA0D1F  wallet_ese_seed.exe

shell_broker.exe is NOT vendored: the one in bin\lpe\ is built from
src\lpe_broker.c in this repository, because the original broker hard-codes a
cmd.exe child while this tool has to launch bin\bypass.exe.

Upstream PoC: CVE-2026-49176 Workspace / "WalletService to SYSTEM"
(see the PoC README and the published write-up).  The binaries are x64; the
broker is built 32-bit and runs fine as a demand-start service.

Only use this on machines you own.  Windows patched after July 2026 does not
have the vulnerability -- the tool then falls back to UAC / manual start.
