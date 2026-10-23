"""SR3 RTX tuner - a separate window that edits the LIVE-SAFE settings in sr3-rtx.ini and asks the
running shim to reload them.

Reload = write the ini, then create sr3-rtx.reload beside the game exe. The shim checks for that
file every 15 frames, re-reads the ini, and drops every baked texture (cloth, hair, skin) so the
next draws rebuild them with the new values. Only settings that are read per draw or per bake are
listed here; anything wired up at startup still needs a game restart.

Run:  pyw tools\\sr3-tuner.pyw  [path\\to\\Saints Row 3]
"""
import os, sys, tkinter as tk
from tkinter import ttk, messagebox

GAME = sys.argv[1] if len(sys.argv) > 1 else r"D:\SR3RTXREMIXCOMP\Saints Row 3"
INI = os.path.join(GAME, "sr3-rtx.ini")
RELOAD = os.path.join(GAME, "sr3-rtx.reload")

# (key, label, kind, lo, hi, default)   kind: "bool" or "int"
SECTIONS = [
    ("Clothing bake", [
        ("generateCloth", "Bake clothing colours", "bool", 0, 1, 1),
        ("clothColourCurve", "Game-style colour curve (Reinhard on Tint)", "bool", 0, 1, 1),
        ("clothAlbedoPercent", "Clothing scale when the curve is off (%)", "int", 0, 500, 200),
        ("clothBrightness", "CPU-baker clothing brightness (%)", "int", 0, 300, 100),
        ("clothUniformFromDiffuse", "Bake flat-pattern garments from their diffuse", "bool", 0, 1, 1),
        ("clothDominantPattern", "Fallback: dominant pattern colour", "bool", 0, 1, 1),
        ("clothMeshDecal", "Decals resolved through the mesh", "bool", 0, 1, 1),
        ("clothDecalTiles", "Tiles for two decals on one island", "bool", 0, 1, 1),
        ("clothCutout", "Alpha cutout (underwear, templates)", "bool", 0, 1, 1),
    ]),
    ("Hair bake", [
        ("hairHsvAlbedo", "Shader-accurate hair colour (HSV)", "bool", 0, 1, 1),
        ("hairStrandsFromDob", "Strand detail (older generator only)", "bool", 0, 1, 1),
        ("hairBrightnessPercent", "Hair brightness (%)", "int", 0, 400, 100),
    ]),
    ("Skin bake", [
        ("skinMatcapAlbedo", "Bake skin tone from the matcaps", "bool", 0, 1, 1),
        ("skinSingleMatcap", "Also the one-matcap (shaderball) form", "bool", 0, 1, 1),
        ("skinFromAtlas", "Bake composited per-character textures", "bool", 0, 1, 0),
        ("skinBrightnessPercent", "Skin brightness (%)", "int", 0, 400, 100),
        ("skinGenPerFrame", "Skin textures built per frame", "int", 0, 16, 2),
    ]),
    ("Other", [
        ("resetTFactorOnTexturedDraws", "Reset texture factor on textured draws", "bool", 0, 1, 1),
    ]),
]


def read_ini():
    with open(INI, "rb") as f:
        return f.read().decode("utf-8", "surrogateescape")


def ini_values(text):
    vals = {}
    for line in text.splitlines():
        if "=" in line and not line.lstrip().startswith(";"):
            k, v = line.split("=", 1)
            vals.setdefault(k.strip(), v.strip())
    return vals


def write_ini(changes):
    text = read_ini()
    nl = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(nl)
    done = set()
    for i, line in enumerate(lines):
        if "=" in line and not line.lstrip().startswith(";"):
            k = line.split("=", 1)[0].strip()
            if k in changes and k not in done:
                lines[i] = "%s=%s" % (k, changes[k])
                done.add(k)
    missing = [k for k in changes if k not in done]
    for k in missing:  # a key the ini lacks is appended rather than silently dropped
        lines.insert(len(lines) - 1 if lines and lines[-1] == "" else len(lines), "%s=%s" % (k, changes[k]))
    with open(INI, "wb") as f:
        f.write(nl.join(lines).encode("utf-8", "surrogateescape"))
    return missing


class Tuner(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("SR3 RTX Tuner")
        self.vars = {}
        if not os.path.exists(INI):
            messagebox.showerror("SR3 RTX Tuner", "No sr3-rtx.ini at\n" + INI)
            self.destroy()
            return
        vals = ini_values(read_ini())
        nb = ttk.Notebook(self)
        nb.pack(fill="both", expand=True, padx=8, pady=8)
        for title, items in SECTIONS:
            page = ttk.Frame(nb, padding=8)
            nb.add(page, text=title)
            for row, (key, label, kind, lo, hi, dflt) in enumerate(items):
                try:
                    cur = int(float(vals.get(key, dflt)))
                except ValueError:
                    cur = dflt
                v = tk.IntVar(value=cur)
                self.vars[key] = v
                if kind == "bool":
                    ttk.Checkbutton(page, text=label, variable=v).grid(row=row, column=0, columnspan=3, sticky="w", pady=2)
                else:
                    ttk.Label(page, text=label).grid(row=row, column=0, sticky="w", pady=2)
                    tk.Scale(page, from_=lo, to=hi, orient="horizontal", variable=v, length=260,
                             showvalue=False).grid(row=row, column=1, padx=6)
                    ttk.Spinbox(page, from_=lo, to=hi, textvariable=v, width=6).grid(row=row, column=2)
        bar = ttk.Frame(self, padding=(8, 0, 8, 8))
        bar.pack(fill="x")
        self.auto = tk.IntVar(value=0)
        ttk.Checkbutton(bar, text="Auto-reload on change", variable=self.auto).pack(side="left")
        ttk.Button(bar, text="Reload in game", command=self.apply).pack(side="right")
        ttk.Button(bar, text="Revert to ini", command=self.revert).pack(side="right", padx=6)
        self.status = ttk.Label(self, text="Game folder: " + GAME, padding=(8, 0, 8, 8))
        self.status.pack(fill="x")
        self._pending = None
        for v in self.vars.values():
            v.trace_add("write", self._changed)

    def _changed(self, *_):
        if self.auto.get():
            if self._pending:
                self.after_cancel(self._pending)
            self._pending = self.after(600, self.apply)   # debounce slider drags

    def apply(self):
        self._pending = None
        try:
            changes = {k: str(int(v.get())) for k, v in self.vars.items()}
        except (tk.TclError, ValueError):
            self.status.config(text="A value is not a number - fix it and press Reload again.")
            return
        try:
            missing = write_ini(changes)
            open(RELOAD, "w").close()
        except OSError as e:
            self.status.config(text="Could not write: %s" % e)
            return
        note = (" (added missing keys: %s)" % ", ".join(missing)) if missing else ""
        self.status.config(text="Saved and reload requested - the game applies it within a moment." + note)

    def revert(self):
        vals = ini_values(read_ini())
        auto = self.auto.get()
        self.auto.set(0)
        for k, v in self.vars.items():
            if k in vals:
                try:
                    v.set(int(float(vals[k])))
                except ValueError:
                    pass
        self.auto.set(auto)
        self.status.config(text="Reverted to the values in the ini.")


if __name__ == "__main__":
    app = Tuner()
    if app.winfo_exists():
        app.mainloop()
