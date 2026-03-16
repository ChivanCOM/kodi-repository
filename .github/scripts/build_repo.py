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

    print(f"  Zipping {addon_dir} v{version}")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        addon_path = os.path.join(ROOT, addon_dir)
        for dirpath, _, filenames in os.walk(addon_path):
            for filename in filenames:
                if filename.endswith(".zip"):
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


if __name__ == "__main__":
    print("Building Kodi repository...")
    for addon_dir in sorted(ADDON_DIRS):
        build_zip(addon_dir)
    build_addons_xml()
    print("Done.")
