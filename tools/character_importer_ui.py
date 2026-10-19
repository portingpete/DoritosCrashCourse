"""Desktop front end for the bounded native character importer."""
from __future__ import annotations

import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from character_importer import find_blender, import_model, install_pack
from character_packs import ROOT, library, resolve_pack, select_pack, selected_pack, validate_pack


class Importer:
    def __init__(self, root):
        self.root = root
        root.title("Crash Course · Character Importer")
        root.geometry("960x720")
        root.minsize(820, 620)
        self.events = queue.Queue()
        self.busy = False
        self.packs = []
        self.last_scene = None
        self.source = tk.StringVar()
        self.name = tk.StringVar()
        self.blender = tk.StringVar()
        self.auto_fit = tk.BooleanVar(value=True)
        self.activate = tk.BooleanVar(value=True)
        self.yaw = tk.StringVar(value="0")
        self.scale = tk.StringVar(value="1")
        self.weights = tk.StringVar(value="surface")
        self.status = tk.StringVar(value="Ready. CJ is the bundled default.")
        try:
            self.blender.set(str(find_blender()))
        except ValueError:
            pass
        style = ttk.Style(root)
        style.theme_use("clam")
        style.configure("TFrame", background="#f5f2eb")
        style.configure("TLabel", background="#f5f2eb", foreground="#252320")
        style.configure("Title.TLabel", font=("Segoe UI", 23, "bold"))
        style.configure("TButton", padding=(12, 7), font=("Segoe UI", 10))
        style.configure("Accent.TButton", background="#ed6c24", foreground="#171717")
        frame = ttk.Frame(root, padding=24)
        frame.pack(fill="both", expand=True)
        frame.columnconfigure(1, weight=1)
        ttk.Label(frame, text="Character Importer", style="Title.TLabel").grid(row=0, column=0, columnspan=3, sticky="w")
        ttk.Label(frame, text="Bring a new runner to Crash Course. CJ is always available to restore.",
                  font=("Segoe UI", 10)).grid(row=1, column=0, columnspan=3, pady=(4, 18), sticky="w")
        self.field(frame, 2, "Model", self.source, self.browse_model)
        self.field(frame, 3, "Name", self.name)
        self.field(frame, 4, "Blender", self.blender, self.browse_blender)
        options = ttk.Frame(frame)
        options.grid(row=5, column=0, columnspan=3, pady=10, sticky="ew")
        ttk.Checkbutton(options, text="Fit height and ground position", variable=self.auto_fit).pack(side="left")
        ttk.Label(options, text="Turn Z°").pack(side="left", padx=(20, 6))
        ttk.Combobox(options, textvariable=self.yaw, values=("0", "90", "180", "-90"), width=6).pack(side="left")
        ttk.Label(options, text="Scale").pack(side="left", padx=(15, 6))
        ttk.Entry(options, textvariable=self.scale, width=6).pack(side="left")
        ttk.Label(options, text="Weights").pack(side="left", padx=(15, 6))
        ttk.Combobox(options, textvariable=self.weights, values=("surface", "existing"), state="readonly", width=9).pack(side="left")
        ttk.Label(frame, text="Best results: an upright T-pose, facing −Y in Blender, with 1–4 materials.\n"
                  "GLB/glTF, FBX and OBJ are supported. Use a fitting scene for manual adjustments.",
                  font=("Segoe UI", 9)).grid(row=6, column=0, columnspan=3, sticky="w", pady=(0, 8))
        actions = ttk.Frame(frame)
        actions.grid(row=7, column=0, columnspan=3, sticky="ew", pady=(0, 16))
        self.import_button = ttk.Button(actions, text="Import character", style="Accent.TButton", command=self.start_import)
        self.import_button.pack(side="left")
        self.prepare_button = ttk.Button(actions, text="Create fitting scene", command=lambda: self.start_import(True))
        self.prepare_button.pack(side="left", padx=8)
        self.scene_button = ttk.Button(actions, text="Open last fitting scene", command=self.open_scene, state="disabled")
        self.scene_button.pack(side="left")
        ttk.Checkbutton(actions, text="Use after importing", variable=self.activate).pack(side="right")
        ttk.Label(frame, text="Installed characters", font=("Segoe UI", 12, "bold")).grid(row=8, column=0, columnspan=3, sticky="w")
        self.listbox = tk.Listbox(frame, height=5, font=("Segoe UI", 11), relief="flat", borderwidth=1,
                                  selectbackground="#ed6c24", selectforeground="#171717", exportselection=False)
        self.listbox.grid(row=9, column=0, columnspan=3, sticky="nsew", pady=8)
        frame.rowconfigure(9, weight=1)
        selection = ttk.Frame(frame)
        selection.grid(row=10, column=0, columnspan=3, sticky="ew")
        self.use_button = ttk.Button(selection, text="Use selected", command=self.use_selected)
        self.use_button.pack(side="left")
        self.restore_button = ttk.Button(selection, text="Restore CJ", command=self.restore_cj)
        self.restore_button.pack(side="left", padx=8)
        self.pack_button = ttk.Button(selection, text="Install pack folder…", command=self.install_folder)
        self.pack_button.pack(side="left")
        ttk.Label(frame, textvariable=self.status, wraplength=860).grid(row=11, column=0, columnspan=3, sticky="ew", pady=(12, 5))
        self.progress = ttk.Progressbar(frame, mode="indeterminate")
        self.progress.grid(row=12, column=0, columnspan=3, sticky="ew")
        self.log = tk.Text(frame, height=7, wrap="word", font=("Consolas", 9), background="#222323", foreground="#e6e4dd", state="disabled")
        self.log.grid(row=13, column=0, columnspan=3, sticky="nsew", pady=(8, 0))
        self.refresh()
        root.after(100, self.poll)

    def field(self, frame, row, label, variable, browse=None):
        ttk.Label(frame, text=label).grid(row=row, column=0, sticky="w", padx=(0, 12), pady=4)
        ttk.Entry(frame, textvariable=variable).grid(row=row, column=1, sticky="ew", pady=4)
        if browse:
            ttk.Button(frame, text="Browse…", command=browse).grid(row=row, column=2, padx=(10, 0), pady=4)

    def browse_model(self):
        value = filedialog.askopenfilename(title="Choose a character model", filetypes=[
            ("Character models", "*.glb *.gltf *.fbx *.obj *.blend"), ("All files", "*.*")])
        if value:
            self.source.set(value)
            self.name.set(Path(value).stem)

    def browse_blender(self):
        value = filedialog.askopenfilename(title="Choose Blender executable", filetypes=[("Blender", "blender.exe"), ("All files", "*.*")])
        if value:
            self.blender.set(value)

    def refresh(self):
        try:
            self.packs = library()
            active = selected_pack().fingerprint
            self.listbox.delete(0, "end")
            for index, pack in enumerate(self.packs):
                suffix = " · default" if pack.metadata["id"] == "cj" else ""
                suffix += " · selected" if pack.fingerprint == active else ""
                self.listbox.insert("end", pack.metadata["name"] + suffix)
                if pack.fingerprint == active:
                    self.listbox.selection_set(index)
        except (OSError, ValueError) as error:
            self.status.set(str(error) + ". Restore CJ to reset selection.")

    def start_import(self, prepare_only=False):
        if self.busy:
            return
        try:
            options = dict(source=Path(self.source.get()), name=self.name.get() or None,
                           blender=Path(self.blender.get()) if self.blender.get() else None,
                           auto_fit=self.auto_fit.get(), yaw=float(self.yaw.get()), scale=float(self.scale.get()),
                           weights=self.weights.get(), prepare_only=prepare_only)
        except ValueError:
            messagebox.showerror("Invalid options", "Rotation and scale must be numbers.")
            return
        activate = self.activate.get()
        self.busy = True
        for button in (self.import_button, self.prepare_button, self.use_button, self.restore_button, self.pack_button):
            button.configure(state="disabled")
        self.progress.start(12)
        self.status.set("Converting in Blender… the game files and saves stay unchanged.")
        def worker():
            try:
                result = import_model(**options, log=lambda text: self.events.put(("log", text)))
                if activate and not result.get("prepared_only"):
                    select_pack(resolve_pack(result["pack"]))
                self.events.put(("done", result))
            except Exception as error:
                self.events.put(("error", str(error)))
        threading.Thread(target=worker, daemon=True).start()

    def poll(self):
        while True:
            try:
                kind, value = self.events.get_nowait()
            except queue.Empty:
                break
            if kind == "log":
                self.log.configure(state="normal")
                self.log.insert("end", value + "\n")
                self.log.see("end")
                self.log.configure(state="disabled")
            else:
                self.busy = False
                self.progress.stop()
                for button in (self.import_button, self.prepare_button, self.use_button, self.restore_button, self.pack_button):
                    button.configure(state="normal")
                if kind == "error":
                    self.status.set("Import failed. Your selected character is unchanged.")
                    messagebox.showerror("Character import failed", value)
                else:
                    self.last_scene = Path(value["fitting_scene"])
                    self.scene_button.configure(state="normal")
                    self.refresh()
                    self.status.set("Fitting scene ready. Adjust it in Blender, then import that .blend file." if value.get("prepared_only") else
                                    f"Imported {value['name']}. Character selection takes effect on the next Launch.cmd run.")
        self.root.after(100, self.poll)

    def choose(self, pack):
        try:
            select_pack(pack)
            self.refresh()
            self.status.set(pack.metadata["name"] + " selected for the next game launch.")
        except (OSError, ValueError) as error:
            messagebox.showerror("Cannot select character", str(error))

    def use_selected(self):
        selection = self.listbox.curselection()
        if selection:
            self.choose(self.packs[selection[0]])

    def restore_cj(self):
        self.choose(resolve_pack("cj"))

    def install_folder(self):
        value = filedialog.askdirectory(title="Choose a folder containing character.json")
        if value:
            try:
                pack = install_pack(validate_pack(Path(value)))
                self.refresh()
                self.status.set("Installed " + pack.metadata["name"] + ". Select it and choose Use selected.")
            except (OSError, ValueError) as error:
                messagebox.showerror("Cannot install pack", str(error))

    def open_scene(self):
        if self.last_scene:
            subprocess.Popen([str(find_blender(Path(self.blender.get()) if self.blender.get() else None)),
                              "--disable-autoexec", str(self.last_scene)], cwd=ROOT)


if __name__ == "__main__":
    root = tk.Tk()
    Importer(root)
    root.mainloop()
