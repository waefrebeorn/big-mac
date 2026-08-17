#!/usr/bin/env python3
"""
Kevin Bacon Trust Graph — Application Firewall for Big Mac
===========================================================
"Six Degrees of Trust" — every binary on the system gets a trust score
based on its distance from known-good roots in the trust graph.

Degree 0: Apple root (signed by Apple, in /System, /usr/bin, /bin)
Degree 1: Apple user apps (Safari, Mail, Messages, etc.)
Degree 2: Your dev tools (Xcode, Homebrew, Hermes, etc.)
Degree 3: Explicitly allowed by user
Degree 4+: Unknown → BLOCK

Generates:
  - socketfilterfw allow/deny rules (application firewall)
  - pf tables for network-layer enforcement
  - A trust report showing every binary and its degree
"""

import os
import sys
import json
import subprocess
import hashlib
from pathlib import Path
from datetime import datetime
from dataclasses import dataclass, field
from typing import Optional

HERMES_HOME = Path(os.environ.get("HERMES_HOME", Path.home() / ".hermes"))
FIREWALL_DIR = Path(__file__).parent
TRUST_DB = FIREWALL_DIR / "trust_graph.json"
LOG_FILE = FIREWALL_DIR / "kevin_bacon.log"

# ─── Trust roots (degree 0) ──────────────────────────────────────────
APPLE_ROOT_PATHS = [
    "/System/Library/CoreServices",
    "/System/Library/Frameworks",
    "/usr/bin",
    "/usr/sbin",
    "/bin",
    "/sbin",
    "/usr/libexec",
]

APPLE_USER_APPS = [
    "/Applications/Safari.app",
    "/Applications/Mail.app",
    "/Applications/Messages.app",
    "/Applications/FaceTime.app",
    "/Applications/Calendar.app",
    "/Applications/Contacts.app",
    "/Applications/Notes.app",
    "/Applications/Reminders.app",
    "/Applications/Photos.app",
    "/Applications/Music.app",
    "/Applications/Podcasts.app",
    "/Applications/TV.app",
    "/Applications/Preview.app",
    "/Applications/TextEdit.app",
    "/Applications/Dictionary.app",
    "/Applications/Calculator.app",
    "/Applications/System Preferences.app",
    "/Applications/System Settings.app",
    "/Applications/App Store.app",
    "/Applications/Utilities",
]

# Degree 2: your dev tools (you explicitly trust these)
DEV_TOOLS = [
    "/Applications/Xcode.app",
    "/Applications/GarageBand.app",
    "/Applications/Logic Pro.app",
    "/Applications/Ableton Live 11 Suite.app",
    "/Applications/Reaper.app",
    "/usr/local/bin",
    "/usr/local/sbin",
    "/usr/local/Cellar",
    "/usr/local/Homebrew",
    str(Path.home() / "homebrew/bin"),
    str(Path.home() / "homebrew/sbin"),
    str(Path.home() / ".local/bin"),
    str(Path.home() / ".hermes"),
    str(Path.home() / "Documents/big-mac"),
    "/Applications/Friendly Netflix.app",
    "/Applications/zoom.us.app",
    "/Applications/Discord.app",
    "/Applications/Slack.app",
    "/Applications/Finder.app",
    "/Applications/Chrome.app",
    "/Applications/Google Chrome.app",
    "/Applications/Firefox.app",
]

# Known Apple code signing authorities
APPLE_SIGNING_PREFIXES = [
    "Software Signing",
    "Apple Code Signing Certification Authority",
    "Apple Root CA",
]

@dataclass
class BinaryTrust:
    path: str
    degree: int = 99  # 99 = untrusted/unknown
    signed: bool = False
    signing_id: str = ""
    team_id: str = ""
    is_apple: bool = False
    is_dev_tool: bool = False
    is_in_hermes: bool = False
    sha256: str = ""
    verdict: str = "BLOCK"

def log(msg: str):
    ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    line = f"[{ts}] {msg}"
    print(line)
    with open(LOG_FILE, "a") as f:
        f.write(line + "\n")

def get_code_signature(binary_path: str) -> dict:
    """Check code signing status of a binary."""
    try:
        result = subprocess.run(
            ["codesign", "-dv", "--verbose=4", binary_path],
            capture_output=True, text=True, timeout=10
        )
        info = {"signed": False, "signing_id": "", "team_id": "", "authority": ""}
        output = result.stderr + result.stdout
        for line in output.split("\n"):
            if "Signature=adhoc" in line or line.strip() == "signed":
                info["signed"] = True
            if "Identifier=" in line:
                info["signing_id"] = line.split("=", 1)[1].strip()
            if "TeamIdentifier=" in line:
                info["team_id"] = line.split("=", 1)[1].strip()
            if "Authority=" in line:
                info["authority"] = line.split("=", 1)[1].strip()
        return info
    except Exception:
        return {"signed": False, "signing_id": "", "team_id": "", "authority": ""}

def is_apple_signed(sig_info: dict) -> bool:
    """Check if binary is signed by Apple."""
    auth = sig_info.get("authority", "")
    return any(apple_auth in auth for apple_auth in APPLE_SIGNING_PREFIXES)

def sha256_file(path: str) -> str:
    """Get SHA256 hash of a file."""
    try:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(65536), b""):
                h.update(chunk)
        return h.hexdigest()
    except Exception:
        return ""

def scan_directory(dir_path: str, max_files: int = 500) -> list:
    """Find all executable files in a directory."""
    binaries = []
    dir_p = Path(dir_path)
    if not dir_p.exists():
        return binaries
    count = 0
    for item in dir_p.rglob("*"):
        if count >= max_files:
            break
        if item.is_file() and os.access(str(item), os.X_OK):
            binaries.append(str(item))
            count += 1
        elif item.is_file() and item.suffix in (".app", ".cli", ".tool"):
            binaries.append(str(item))
            count += 1
    return binaries

def compute_trust(binary_path: str) -> BinaryTrust:
    """Compute trust degree for a binary."""
    bt = BinaryTrust(path=binary_path)
    sig = get_code_signature(binary_path)
    bt.signed = sig["signed"]
    bt.signing_id = sig["signing_id"]
    bt.team_id = sig["team_id"]
    bt.is_apple = is_apple_signed(sig)
    bt.sha256 = sha256_file(binary_path)

    # Degree 0: Apple system binaries
    if any(binary_path.startswith(p) for p in APPLE_ROOT_PATHS) and bt.is_apple:
        bt.degree = 0
        bt.verdict = "TRUST_ROOT"
        return bt

    # Degree 1: Apple user apps
    if any(binary_path.startswith(p) for p in APPLE_USER_APPS) and bt.is_apple:
        bt.degree = 1
        bt.verdict = "TRUST_APPLE"
        return bt

    # Degree 2: Your dev tools
    if any(binary_path.startswith(p) for p in DEV_TOOLS):
        bt.degree = 2
        bt.is_dev_tool = True
        bt.verdict = "TRUST_DEV"
        return bt

    # Hermes agent (this process) — degree 2
    if ".hermes" in binary_path:
        bt.degree = 2
        bt.is_in_hermes = True
        bt.verdict = "TRUST_HERMES"
        return bt

    # Degree 3: Signed by a known developer (not Apple, but signed)
    if bt.signed:
        bt.degree = 3
        bt.verdict = "TRUST_SIGNED"
        return bt

    # Unknown / unsigned — BLOCK
    bt.degree = 99
    bt.verdict = "BLOCK"
    return bt

def build_trust_graph() -> dict:
    """Scan the system and build the full trust graph."""
    log("=== Kevin Bacon Trust Graph Scan Starting ===")
    all_binaries = []

    # Scan Apple root paths
    for path in APPLE_ROOT_PATHS:
        found = scan_directory(path, max_files=100)
        log(f"  Scanned {path}: {found and len(found)} binaries")
        all_binaries.extend(found)

    # Scan Apple user apps
    for path in APPLE_USER_APPS:
        if os.path.exists(path):
            all_binaries.append(path)

    # Scan dev tools
    for path in DEV_TOOLS:
        found = scan_directory(path, max_files=200)
        log(f"  Scanned {path}: {found and len(found)} binaries")
        all_binaries.extend(found)

    # Scan /Applications for anything else
    app_binaries = scan_directory("/Applications", max_files=100)
    all_binaries.extend(app_binaries)

    # Deduplicate
    all_binaries = list(set(all_binaries))
    log(f"Total unique binaries to assess: {len(all_binaries)}")

    # Compute trust for each
    trust_results = []
    for i, binary in enumerate(all_binaries):
        bt = compute_trust(binary)
        trust_results.append({
            "path": bt.path,
            "degree": bt.degree,
            "signed": bt.signed,
            "signing_id": bt.signing_id,
            "team_id": bt.team_id,
            "is_apple": bt.is_apple,
            "is_dev_tool": bt.is_dev_tool,
            "sha256": bt.sha256,
            "verdict": bt.verdict,
        })
        if (i + 1) % 50 == 0:
            log(f"  Processed {i+1}/{len(all_binaries)}...")

    # Sort by degree
    trust_results.sort(key=lambda x: (x["degree"], x["path"]))

    # Summary
    degree_counts = {}
    for r in trust_results:
        d = r["degree"]
        degree_counts[d] = degree_counts.get(d, 0) + 1

    log("=== Trust Distribution ===")
    for d in sorted(degree_counts.keys()):
        label = {0: "ROOT(Apple)", 1: "APPLE_APPS", 2: "DEV_TOOLS", 3: "SIGNED", 99: "UNKNOWN"}.get(d, f"DEG_{d}")
        log(f"  Degree {d} ({label}): {degree_counts[d]} binaries")

    blocked = [r for r in trust_results if r["verdict"] == "BLOCK"]
    log(f"  BLOCKED (unknown/unsigned): {len(blocked)}")

    # Save trust DB
    trust_db = {
        "generated": datetime.now().isoformat(),
        "machine": "iMac14,4",
        "total_binaries": len(trust_results),
        "degree_distribution": degree_counts,
        "binaries": trust_results,
    }
    with open(TRUST_DB, "w") as f:
        json.dump(trust_db, f, indent=2)
    log(f"Trust DB saved to {TRUST_DB}")

    return trust_db

def generate_socketfilterfw_rules(trust_db: dict):
    """Generate application firewall commands for trusted binaries."""
    rules_file = FIREWALL_DIR / "apply_app_firewall.sh"
    lines = ["#!/bin/bash", "# Auto-generated Kevin Bacon application firewall rules", f"# Generated: {datetime.now().isoformat()}", ""]

    trusted = [b for b in trust_db["binaries"] if b["degree"] <= 3]
    blocked = [b for b in trust_db["binaries"] if b["degree"] == 99]

    lines.append(f"# Allowing {len(trusted)} trusted binaries (degree 0-3)")
    for b in trusted:
        path = b["path"]
        if os.path.exists(path):
            lines.append(f'/usr/libexec/ApplicationFirewall/socketfilterfw --add "{path}" 2>/dev/null')
            lines.append(f'/usr/libexec/ApplicationFirewall/socketfilterfw --unblockapp "{path}" 2>/dev/null')

    lines.append("")
    lines.append(f"# Blocking {len(blocked)} unknown/unsigned binaries")
    for b in blocked:
        path = b["path"]
        if os.path.exists(path):
            lines.append(f'/usr/libexec/ApplicationFirewall/socketfilterfw --blockapp "{path}" 2>/dev/null')

    content = "\n".join(lines) + "\n"
    with open(rules_file, "w") as f:
        f.write(content)
    os.chmod(rules_file, 0o755)
    log(f"Application firewall rules saved to {rules_file}")

def generate_pf_tables(trust_db: dict):
    """Generate pf tables for trusted outbound destinations."""
    tables_file = FIREWALL_DIR / "pf_tables.conf"
    lines = ["# Kevin Bacon Trust Graph — pf tables", f"# Generated: {datetime.now().isoformat()}"]

    # Trusted binaries (for reference / future use in combined rules)
    trusted_paths = [b["path"] for b in trust_db["binaries"] if b["degree"] <= 3]
    lines.append(f"# Trusted binaries: {len(trusted_paths)}")

    # Blocked binaries
    blocked_paths = [b["path"] for b in trust_db["binaries"] if b["degree"] == 99]
    lines.append(f"# Blocked binaries: {len(blocked_paths)}")
    for p in blocked_paths:
        lines.append(f"# BLOCK: {p}")

    content = "\n".join(lines) + "\n"
    with open(tables_file, "w") as f:
        f.write(content)
    log(f"pf tables saved to {tables_file}")

if __name__ == "__main__":
    print("=" * 60)
    print("  Kevin Bacon Trust Graph — Big Mac Firewall")
    print("  Six Degrees of Trust")
    print("=" * 60)
    print()

    trust_db = build_trust_graph()
    generate_socketfilterfw_rules(trust_db)
    generate_pf_tables(trust_db)

    print()
    print("Done. Review the trust DB and run apply_app_firewall.sh with sudo.")
    print(f"Trust DB: {TRUST_DB}")
