#zwo2awo makes AntWorkout workout files (.awo) from zwift workout files (.zwo), each beside its .zwo
#   python zwo2awo.py                      every .zwo in the current folder
#   python zwo2awo.py <folder or file> ... those .zwo files (a folder's .zwo files)
#the conversion itself is ZwoConvert.py, which the app also uses to ride a .zwo file directly
#(the app's workout picker can convert them too: "Convert to .awo")

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ZwoConvert


def zwo_files(arguments):
    files = []
    for argument in arguments or ["."]:
        if os.path.isdir(argument):
            files += [os.path.join(argument, f) for f in sorted(os.listdir(argument)) if f.lower().endswith(".zwo")]
        else:
            files.append(argument)
    return files


if __name__ == "__main__":
    failed = 0
    for path in zwo_files(sys.argv[1:]):
        try:
            awoPath, warnings = ZwoConvert.write_awo(path)
        except ZwoConvert.ZwoError as e:
            failed += 1
            print(f"{path}: not converted, {e}")
            continue
        print(awoPath)
        for warning in warnings:
            print(f"    {warning}")
    sys.exit(1 if failed else 0)
