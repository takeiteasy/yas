import sys
import os

if len(sys.argv) < 3:
    print("Usage: python embed.py <outfile> <src_files...>")
    sys.exit(1)

outfile = sys.argv[1]
src_files = sys.argv[2:]

# Start generating C header
header_content = []
header_content.append("#ifndef STDLIB_H")
header_content.append("#define STDLIB_H")
header_content.append("")
header_content.append("typedef struct {")
header_content.append("    const char *name;")
header_content.append("    const char *source;")
header_content.append("} StdlibFile;")
header_content.append("")

# Generate variables for each file content to avoid huge inline strings in array
file_entries = []

for i, src in enumerate(src_files):
    if src == "embed.py": continue # simple check in case of makefile $^
    
    try:
        with open(src, "r") as f:
            content = f.read()
    except Exception as e:
        print(f"Error reading {src}: {e}")
        sys.exit(1)

    # Determine internal name (key)
    # logic: strip "std/" prefix if present, strip ".ys" suffix
    # e.g. std/yas/base.ys -> yas/base
    
    key = src
    if key.startswith("std/"):
        key = key[4:]
    if key.endswith(".ys"):
        key = key[:-3]
        
    # Escape content
    escaped_lines = []
    for line in content.splitlines():
        esc = line.replace('\\', '\\\\').replace('"', '\\"')
        escaped_lines.append(f'"{esc}\\n"')
    
    if not escaped_lines:
        c_content = '""'
    else:
        c_content = "\n".join(escaped_lines)
        
    var_name = f"stdlib_src_{i}"
    header_content.append(f"static const char {var_name}[] = \n{c_content};\n")
    
    file_entries.append(f'    {{ "{key}", {var_name} }},')

header_content.append("static const StdlibFile stdlib_files[] = {")
header_content.extend(file_entries)
header_content.append("    { NULL, NULL }")
header_content.append("};")
header_content.append("")
header_content.append("#endif")

with open(outfile, "w") as f:
    f.write("\n".join(header_content))
