"""
Builds the Kodi repository:
  - Creates a zip for every addon that has changed (or is missing a zip)
  - Regenerates addons.xml and addons.xml.md5
"""

import hashlib
import os
import re
import zipfile
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

ADDON_DIRS = [
    d for d in os.listdir(ROOT)
    if os.path.isdir(os.path.join(ROOT, d))
    and os.path.exists(os.path.join(ROOT, d, "addon.xml"))
    and not d.startswith(".")
]


def get_version(addon_dir):
    tree = ET.parse(os.path.join(ROOT, addon_dir, "addon.xml"))
    return tree.getroot().attrib["version"]


def build_zip(addon_dir):
    version = get_version(addon_dir)
    zip_name = f"{addon_dir}-{version}.zip"
    zip_path = os.path.join(ROOT, addon_dir, zip_name)

    # Remove stale zips for this addon
    for f in os.listdir(os.path.join(ROOT, addon_dir)):
        if f.endswith(".zip") and f != zip_name:
            os.remove(os.path.join(ROOT, addon_dir, f))

    if os.path.exists(zip_path):
        return  # already up to date

    # Directories and file extensions to exclude from every zip
    SKIP_DIRS = {"build", "src", "__pycache__", ".git"}
    SKIP_EXTS = {".zip", ".cpp", ".h", ".sh"}
    SKIP_FILES = {"CMakeLists.txt"}

    print(f"  Zipping {addon_dir} v{version}")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        addon_path = os.path.join(ROOT, addon_dir)
        for dirpath, dirnames, filenames in os.walk(addon_path):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for filename in filenames:
                if filename in SKIP_FILES:
                    continue
                if os.path.splitext(filename)[1] in SKIP_EXTS:
                    continue
                full_path = os.path.join(dirpath, filename)
                arcname = os.path.relpath(full_path, ROOT)
                zf.write(full_path, arcname)


def build_addons_xml():
    addons_node = ET.Element("addons")
    for addon_dir in sorted(ADDON_DIRS):
        tree = ET.parse(os.path.join(ROOT, addon_dir, "addon.xml"))
        addons_node.append(tree.getroot())

    ET.indent(addons_node, space="  ")
    content = '<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(
        addons_node, encoding="unicode"
    ) + "\n"

    with open(os.path.join(ROOT, "addons.xml"), "w", encoding="utf-8") as f:
        f.write(content)

    md5 = hashlib.md5(content.encode("utf-8")).hexdigest()
    with open(os.path.join(ROOT, "addons.xml.md5"), "w") as f:
        f.write(md5)

    print(f"  addons.xml written ({len(ADDON_DIRS)} addons), md5={md5}")


def build_index_html():
    """Generate index.html files so Kodi's HTTP browser can navigate the repo."""
    # Root index — lists addon subdirectories
    root_links = "\n".join(
        f'    <a href="{d}/">{d}/</a><br>' for d in sorted(ADDON_DIRS)
    )
    root_html = f"""<!DOCTYPE html>
<html><body>
<h1>iBroadcast Kodi Repository</h1>
{root_links}
</body></html>
"""
    with open(os.path.join(ROOT, "index.html"), "w") as f:
        f.write(root_html)

    # Per-addon index — lists the zip file
    for addon_dir in ADDON_DIRS:
        version = get_version(addon_dir)
        zip_name = f"{addon_dir}-{version}.zip"
        addon_html = f"""<!DOCTYPE html>
<html><body>
<h1>{addon_dir}</h1>
    <a href="{zip_name}">{zip_name}</a><br>
</body></html>
"""
        with open(os.path.join(ROOT, addon_dir, "index.html"), "w") as f:
            f.write(addon_html)

    print(f"  index.html files written")


if __name__ == "__main__":
    print("Building Kodi repository...")
    for addon_dir in sorted(ADDON_DIRS):
        build_zip(addon_dir)
    build_addons_xml()
    build_index_html()
    print("Done.")
