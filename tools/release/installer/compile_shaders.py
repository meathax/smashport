"""Build the D3D11 shaders embedded in both Windows player build paths."""
import argparse
from pathlib import Path
import subprocess
import tempfile

SOURCE = Path(__file__).resolve().parents[3] / "src/host"
COMPUTE = ("integrate", "spawn", "binParticles", "composite", "markHud",
           "markReceiver", "stepSmoke", "stampBodies", "giScene", "giCascade")


def compile_shaders(fxc, output):
    header = ["#pragma once", "#include <cstddef>", "namespace resident_shader {"]
    stages = [(entry, "cs", "effects") for entry in COMPUTE]
    stages.extend([("vs", "vs", "display"), ("ps", "ps", "display"),
                   ("overlayPS", "ps", "display"),
                   ("menuScenePS", "ps", "menu"), ("menuBlitPS", "ps", "menu"),
                   ("menuGlowPS", "ps", "menu")])
    with tempfile.TemporaryDirectory() as scratch:
        blob = Path(scratch) / "shader.cso"
        for entry, stage, source in stages:
            subprocess.run([fxc, "/nologo", "/O3", "/T", stage + "_5_0",
                            "/E", entry, "/Fo", blob, SOURCE / f"resident_{source}.hlsl"],
                           check=True)
            data = blob.read_bytes()
            header.append(f"inline constexpr unsigned char {entry}[] = {{")
            for pos in range(0, len(data), 24):
                header.append(",".join(map(str, data[pos:pos+24])) + ",")
            header.append("};")
    header.extend(["struct Code { const void* data; std::size_t size; const char* name; };",
                   "inline constexpr Code compute[] = {"])
    header.extend(f'{{{entry}, sizeof({entry}), "{entry}"}},' for entry in COMPUTE)
    header.extend(["};", "}"])
    output.write_text("\n".join(header) + "\n", encoding="ascii")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fxc", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    compile_shaders(args.fxc, args.output)
