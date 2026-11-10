"""
app.py - Flask backend.
It does NOT contain any algorithm. It only:
  1. receives a request from the browser,
  2. runs the C program with arguments,
  3. converts the C output (plain text lines) into JSON.
"""
import os
import re
import time
import shutil
import threading
import hmac
import subprocess
from functools import wraps
from werkzeug.exceptions import HTTPException
from flask import Flask, jsonify, request, render_template, session

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINE = os.path.join(BASE_DIR, "engine", "engine.exe" if os.name == "nt" else "engine")

app = Flask(
    __name__,
    template_folder=os.path.join(BASE_DIR, "frontend", "templates"),
    static_folder=os.path.join(BASE_DIR, "frontend", "static"),
)


# ---------- Data files (self-healing) ----------
DATA_DIR = os.path.join(BASE_DIR, "data")
WORDS_FILE = os.path.join(DATA_DIR, "words.txt")
DEFAULT_WORDS = os.path.join(BASE_DIR, "backend", "words_default.txt")   # backup copy, never changed
BIGRAM_FILE = os.path.join(DATA_DIR, "bigrams.txt")
TEMP_FILE = os.path.join(DATA_DIR, "temp.txt")


def ensure_data_files():
    """Make sure data/words.txt exists and is not empty, so the dictionary is never lost."""
    os.makedirs(DATA_DIR, exist_ok=True)
    words_missing = not os.path.exists(WORDS_FILE) or os.path.getsize(WORDS_FILE) == 0
    if words_missing and os.path.exists(TEMP_FILE) and os.path.getsize(TEMP_FILE) > 0:
        shutil.copyfile(TEMP_FILE, WORDS_FILE)        # an interrupted save: recover it
    elif words_missing and os.path.exists(DEFAULT_WORDS):
        shutil.copyfile(DEFAULT_WORDS, WORDS_FILE)    # start again from the default words
    if not os.path.exists(BIGRAM_FILE):
        open(BIGRAM_FILE, "a").close()


# ---------- Admin login settings ----------
# Change the password here, or set the ADMIN_PASSWORD environment variable.
ADMIN_PASSWORD = os.environ.get("ADMIN_PASSWORD", "admin123")
app.secret_key = os.environ.get("SECRET_KEY", "change-this-secret-key")


def admin_required(func):
    """Decorator: only allow the request if the admin is logged in."""
    @wraps(func)
    def wrapper(*args, **kwargs):
        if not session.get("admin"):
            return jsonify({"ok": False, "auth": False, "message": "Admin login required."}), 401
        return func(*args, **kwargs)
    return wrapper


# only one C program may touch the data files at a time
engine_lock = threading.Lock()


def run_engine(*args):
    """Run the C program and return its output as a list of lines."""
    with engine_lock:
        ensure_data_files()
        result = subprocess.run(
            [ENGINE] + [str(a) for a in args],
            cwd=BASE_DIR,            # so the C program finds data/words.txt
            capture_output=True,
            text=True,
            timeout=5,
        )
    if result.returncode != 0:
        raise RuntimeError("The C engine crashed (code %s). %s" % (result.returncode, result.stderr.strip()))
    return result.stdout.strip().splitlines()


def clean(word):
    """Keep only letters a-z (also protects the C program from bad input)."""
    return re.sub(r"[^a-z]", "", word.lower())[:40]


def get_k(default=5):
    try:
        return max(1, min(int(request.args.get("k", default)), 20))
    except ValueError:
        return default


def to_items(lines):
    """'program 120 0'  ->  {"word": "program", "freq": 120, "dist": 0}"""
    items = []
    for line in lines:
        parts = line.split()
        if len(parts) == 3:
            items.append({"word": parts[0], "freq": int(parts[1]), "dist": int(parts[2])})
    return items


@app.errorhandler(Exception)
def show_error(error):
    """Send every problem to the browser as readable JSON (it shows a red message)."""
    if isinstance(error, HTTPException):
        return error                                  # normal 404, 405 ... stay as they are
    if isinstance(error, FileNotFoundError):
        message = "C engine not found. Compile engine/engine.c first (see README)."
    else:
        message = str(error)
    return jsonify({"error": message}), 500


@app.route("/")
def home():
    return render_template("index.html")


@app.route("/api/suggest")
def suggest():
    prefix = clean(request.args.get("prefix", ""))
    if prefix == "":
        return jsonify({"results": []})
    lines = run_engine("suggest", prefix, get_k(6))
    return jsonify({"results": to_items(lines)})


@app.route("/api/spell")
def spell():
    word = clean(request.args.get("word", ""))
    if word == "":
        return jsonify({"correct": True, "results": []})
    lines = run_engine("spell", word, get_k(5))
    correct = len(lines) > 0 and lines[0] == "EXACT"
    return jsonify({"correct": correct, "results": to_items(lines[1:])})


@app.route("/api/next")
def next_word():
    word = clean(request.args.get("word", ""))
    if word == "":
        return jsonify({"results": []})
    lines = run_engine("next", word, get_k(5))
    return jsonify({"results": to_items(lines)})


@app.route("/api/autolearn", methods=["POST"])
def autolearn():
    """Called by the browser every time the user finishes a word."""
    data = request.get_json(silent=True) or {}
    word = clean(str(data.get("word", "")))
    prev = clean(str(data.get("prev", "")))
    if len(word) < 2:
        return jsonify({"status": "IGNORED"})
    lines = run_engine("autolearn", word, prev or "-")
    return jsonify({"status": lines[0] if lines else "IGNORED"})


@app.route("/api/dictionary", methods=["GET"])
@admin_required
def dictionary_list():
    q = clean(request.args.get("q", ""))
    lines = run_engine("list", q)
    words = []
    for line in lines[:500]:
        parts = line.split()
        if len(parts) == 2:
            words.append({"word": parts[0], "freq": int(parts[1])})
    return jsonify({"results": words})


@app.route("/api/dictionary", methods=["POST"])
@admin_required
def dictionary_add():
    data = request.get_json(silent=True) or {}
    word = clean(str(data.get("word", "")))
    try:
        freq = max(1, int(data.get("freq", 1)))
    except (ValueError, TypeError):
        freq = 1
    if word == "":
        return jsonify({"ok": False, "message": "Enter a valid word (letters only)."})
    lines = run_engine("add", word, freq)
    if lines and lines[0] == "OK":
        return jsonify({"ok": True, "message": "Added '%s'." % word})
    return jsonify({"ok": False, "message": lines[0].replace("ERROR ", "") if lines else "The C engine gave no answer."})


@app.route("/api/dictionary/restore", methods=["POST"])
@admin_required
def dictionary_restore():
    """Add every word of backend/words_default.txt that is missing from the dictionary."""
    if not os.path.exists(DEFAULT_WORDS):
        return jsonify({"ok": False, "message": "backend/words_default.txt not found."})
    with engine_lock:
        ensure_data_files()
        with open(WORDS_FILE) as f:
            text = f.read()
        have = set(line.split()[0].lower() for line in text.splitlines() if line.split())
        missing = []
        with open(DEFAULT_WORDS) as f:
            for line in f:
                parts = line.split()
                if parts and parts[0].lower() not in have:
                    missing.append(line.strip())
        with open(WORDS_FILE, "a") as f:
            if text and not text.endswith("\n"):
                f.write("\n")
            if missing:
                f.write("\n".join(missing) + "\n")
    return jsonify({"ok": True, "message": "Restored %d default words." % len(missing)})


@app.route("/api/dictionary/<word>", methods=["DELETE"])
@admin_required
def dictionary_delete(word):
    lines = run_engine("delete", clean(word))
    if lines and lines[0] == "OK":
        return jsonify({"ok": True, "message": "Deleted."})
    return jsonify({"ok": False, "message": "Word not found."})


@app.route("/api/admin/login", methods=["POST"])
def admin_login():
    password = str((request.get_json(silent=True) or {}).get("password", ""))
    # compare_digest compares safely (no timing leak)
    if hmac.compare_digest(password.encode(), ADMIN_PASSWORD.encode()):
        session["admin"] = True
        return jsonify({"ok": True})
    time.sleep(0.7)                      # slows down password guessing
    return jsonify({"ok": False, "message": "Wrong password."}), 401


@app.route("/api/admin/logout", methods=["POST"])
def admin_logout():
    session.pop("admin", None)
    return jsonify({"ok": True})


@app.route("/api/admin/status")
def admin_status():
    return jsonify({"admin": bool(session.get("admin"))})


if __name__ == "__main__":
    ensure_data_files()
    if not os.path.exists(ENGINE):
        print("WARNING: C engine not found at %s - compile engine/engine.c first." % ENGINE)
    app.run(debug=True)
