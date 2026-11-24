"""Compile DXVK's built-in GLSL shaders to embedded SPIR-V headers (Vulkan 1.1)
and validate them.   shaders.py <prepared dxvk dir>"""
import pathlib, subprocess, sys
root = pathlib.Path(sys.argv[1]).resolve()
out = root / "generated"
out.mkdir(exist_ok=True)
n = 0
for d in ("src/dxvk/shaders", "src/dxvk/hud/shaders", "src/d3d11/shaders"):
    for sh in sorted((root / d).iterdir()):
        if sh.suffix not in (".comp", ".frag", ".vert", ".geom"):
            continue
        spv = out / (sh.stem + ".spv")
        subprocess.run(["glslangValidator", "--quiet", "--target-env", "vulkan1.1", str(sh), "-o", str(spv)], check=True)
        subprocess.run(["spirv-val", "--target-env", "vulkan1.1", str(spv)], check=True)
        subprocess.run(["glslangValidator", "--quiet", "--target-env", "vulkan1.1", "--vn", sh.stem, str(sh), "-o", str(out / (sh.stem + ".h"))], check=True)
        n += 1
print(f"{n} built-in shaders generated and validated for Vulkan 1.1")
