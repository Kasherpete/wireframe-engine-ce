# scene.py <name> main.c: replaces the active coordinates[] in a copy of main.c with a generated
# scene. "far": a 9x9 floor and 160 single blocks on a 6-unit grid out to +-36 (241 boxes)
import sys, re
name, p = sys.argv[1], sys.argv[2]
recs = []
if name == "far":
    for x in range(-4, 5):
        for z in range(-4, 5):
            recs.append((x, 1, z, 3 if (x + z) % 2 == 0 else 11))
    mats = [4, 6, 8, 9, 12]
    vals = range(-36, 37, 6)
    k = 0
    for x in vals:
        for z in vals:
            if abs(x) <= 6 and abs(z) <= 6:
                continue
            recs.append((x, 0, z, mats[k % 5])); k += 1
assert len(recs) <= 255, len(recs)
body = "static int8_t coordinates[][5] = {\n" + ",\n".join("    {%d, %d, %d, %d}" % r for r in recs) + "\n};\n"
s = open(p).read()
m = re.search(r"^static int8_t coordinates\[\]\[5\] = \{.*?^\};\n", s, re.S | re.M)
s = s[:m.start()] + body + s[m.end():]
open(p, "w").write(s)
print(len(recs), "records")
