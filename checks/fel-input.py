"""Check SRAM preservation and failure stopping without contacting a board."""
import importlib.util
import json
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("input_check", ROOT / "tools/check-fel-input.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def check(case, samples, fault=None):
    rom = bytes((i * 17 + 5) & 255 for i in range(0x11000))
    memory = bytearray((i * 31 + 9) & 255 for i in range(0x80000))
    saved = bytes(memory)
    calls, sampled = [], []
    failed = False

    def transaction(*arguments):
        nonlocal failed
        if failed:
            raise AssertionError("request after transport failure")
        calls.append(arguments)
        if fault == "transport" and arguments[0] == "memmove":
            failed = True
            raise RuntimeError("injected transport failure")
        output = []
        index = 0
        while index < len(arguments):
            command, *rest = arguments[index:]
            if command == "read":
                address, length, path = int(rest[0], 0), int(rest[1]), rest[2]
                data = bytes(memory[address:address + length])
                if fault == "restoration" and str(path).endswith("copy-after.bin"):
                    data = b"\xff" + data[1:]
                path.write_bytes(data)
                index += 4
            elif command == "write":
                address, path = int(rest[0], 0), rest[1]
                data = path.read_bytes()
                if (address, len(data)) not in [(a, n) for _, a, n in module.REGIONS]:
                    raise AssertionError("write outside reserved SRAM")
                memory[address:address + len(data)] = data
                index += 3
            elif command == "memmove":
                destination, source, length = int(rest[0], 0), int(rest[1], 0), int(rest[2])
                if destination != 0x52000 or length > 0x400:
                    raise AssertionError("copy outside reserved SRAM")
                memory[destination:destination + length] = rom[source:source + length]
                if fault == "rom":
                    memory[destination] ^= 1
                memory[0x71000:0x71080] = bytes([0xaa]) * 128
                index += 4
            elif command == "readl":
                if rest[0] != "0x03000024":
                    raise AssertionError("unexpected MMIO read")
                output.append(hex(samples[len(sampled)]))
                sampled.append(samples[len(sampled)])
                memory[0x71000:0x71080] = bytes([0xbb]) * 128
                index += 2
            else:
                raise AssertionError(f"unexpected FEL command {command}")
        return "malformed" if output and fault == "text" else "\n".join(output)

    with tempfile.TemporaryDirectory() as directory:
        try:
            result = module.inspect_input(transaction, Path(directory), rom)
        except module.InspectionRefused:
            assert fault in ("rom", "text")
            assert memory == saved
            if fault == "rom":
                assert not sampled
        except RuntimeError:
            assert fault in ("transport", "restoration")
            if fault == "transport":
                assert len(calls) == 2 and failed
        else:
            assert fault is None and result == samples and memory == saved
    return {"case": case, "transactions": len(calls), "samples": sampled}


cases = [check("asserted input and SRAM restoration", [1] * 4),
         check("one high input and SRAM restoration", [1, 1, 0x101, 1]),
         check("ROM mismatch refuses sampling and restores SRAM", [], "rom"),
         check("transport failure stops requests", [], "transport"),
         check("malformed response follows SRAM restoration", [1] * 4, "text"),
         check("restoration mismatch fails inspection", [1] * 4, "restoration")]
print(json.dumps({"cases": cases, "limitation": "Synthetic host transport; no board access."}, indent=2))
