#!/usr/bin/env python3
"""
Net-Mask Sweep Detector — watches for port scans, SYN floods, and
reconnaissance against the iMac. Auto-bans offenders via pf tables.

Monitors:
  - pflog0 interface (pf log)
  - Multiple SYN to different ports = scan
  - ICMP sweep = ping sweep
  - Connection attempts to closed ports = recon

Auto-response:
  - Add offender to <scanners> pf table (permanent ban)
  - Log to file with timestamp
  - Optional: alert via Hermes gateway
"""

import subprocess
import re
import time
import signal
import sys
from datetime import datetime
from pathlib import Path
from collections import defaultdict
from dataclasses import dataclass, field

FIREWALL_DIR = Path(__file__).parent
LOG_FILE = FIREWALL_DIR / "sweep_detector.log"
BAN_FILE = FIREWALL_DIR / "banned_hosts.json"

# Tuning
SCAN_THRESHOLD = 5        # unique dst ports from one src in window = scan
SYN_THRESHOLD = 20        # SYN packets in window = flood
ICMP_THRESHOLD = 10       # ICMP from one src in window = ping sweep
WINDOW_SECONDS = 60       # sliding window for detection
CHECK_INTERVAL = 5        # seconds between log polls
BAN_DURATION = 86400      # 24 hours (0 = permanent)

@dataclass
class HostActivity:
    src_ip: str
    dst_ports: set = field(default_factory=set)
    syn_count: int = 0
    icmp_count: int = 0
    first_seen: float = 0.0
    last_seen: float = 0.0
    banned: bool = False

hosts: dict[str, HostActivity] = {}
banned_hosts: dict[str, str] = {}  # ip -> timestamp

def log(msg: str):
    ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    line = f"[{ts}] {msg}"
    print(line, flush=True)
    with open(LOG_FILE, "a") as f:
        f.write(line + "\n")

def ban_ip(ip: str, reason: str):
    """Add IP to pf <scanners> table (drops all traffic from them)."""
    if ip in banned_hosts:
        return
    if ip.startswith("192.168.1."):
        return  # never ban LAN
    if ip == "127.0.0.1":
        return

    try:
        subprocess.run(
            ["sudo", "-n", "pfctl", "-t", "scanners", "-T", "add", ip],
            capture_output=True, text=True, timeout=10
        )
        banned_hosts[ip] = datetime.now().isoformat()
        with open(BAN_FILE, "w") as f:
            import json
            json.dump(banned_hosts, f, indent=2)
        log(f"BANNED: {ip} — {reason}")
    except Exception as e:
        log(f"ERROR banning {ip}: {e}")

def parse_pflog_line(line: str) -> dict:
    """Parse a pflog tcpdump line for relevant fields."""
    info = {"src": "", "dst": "", "proto": "", "dport": 0, "flags": ""}

    # Typical pflog format:
    # rule match, pass/block, in/out, interface, src port > dst port flags
    src_match = re.search(r'(\d+\.\d+\.\d+\.\d+)\.(\d+)\s*>\s*(\d+\.\d+\.\d+\.\d+)\.(\d+)', line)
    if src_match:
        info["src"] = src_match.group(1)
        info["sport"] = int(src_match.group(2))
        info["dst"] = src_match.group(3)
        info["dport"] = int(src_match.group(4))

    if "proto TCP" in line or "tcp" in line.lower():
        info["proto"] = "tcp"
    elif "proto UDP" in line or "udp" in line.lower():
        info["proto"] = "udp"
    elif "proto ICMP" in line or "icmp" in line.lower():
        info["proto"] = "icmp"

    flags_match = re.search(r'\[([A-Z*]+)\]', line)
    if flags_match:
        info["flags"] = flags_match.group(1)

    return info

def check_scan_detection(current_time: float):
    """Check accumulated activity for scan patterns."""
    to_remove = []
    for ip, activity in hosts.items():
        age = current_time - activity.first_seen
        if age > WINDOW_SECONDS:
            # Window expired — evaluate before removing
            if not activity.banned:
                if len(activity.dst_ports) >= SCAN_THRESHOLD:
                    ban_ip(ip, f"port scan ({len(activity.dst_ports)} ports in {WINDOW_SECONDS}s)")
                    activity.banned = True
                elif activity.syn_count >= SYN_THRESHOLD:
                    ban_ip(ip, f"SYN flood ({activity.syn_count} SYNs in {WINDOW_SECONDS}s)")
                    activity.banned = True
                elif activity.icmp_count >= ICMP_THRESHOLD:
                    ban_ip(ip, f"ping sweep ({activity.icmp_count} ICMP in {WINDOW_SECONDS}s)")
                    activity.banned = True
            to_remove.append(ip)
        else:
            # Real-time detection for aggressive scanners
            if len(activity.dst_ports) >= SCAN_THRESHOLD * 2 and not activity.banned:
                ban_ip(ip, f"AGGRESSIVE scan ({len(activity.dst_ports)} ports)")
                activity.banned = True

    for ip in to_remove:
        del hosts[ip]

def tail_pflog():
    """Tail pflog0 interface and detect scans."""
    log("Starting pflog0 monitor...")
    proc = None
    try:
        proc = subprocess.Popen(
            ["sudo", "-n", "tcpdump", "-n", "-e", "-ttt", "-i", "pflog0"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            bufsize=1
        )
        log("Connected to pflog0. Monitoring for scans...")

        while proc is not None and proc.stdout is not None:
            line = proc.stdout.readline()
            if not line:
                break
            line = line.strip()
            if not line:
                continue

            info = parse_pflog_line(line)
            src = info.get("src", "")
            if not src or src.startswith("192.168.1.") or src == "127.0.0.1":
                continue

            current_time = time.time()
            if src not in hosts:
                hosts[src] = HostActivity(src_ip=src, first_seen=current_time)

            activity = hosts[src]
            activity.last_seen = current_time

            if info["proto"] == "tcp":
                if "S" in info.get("flags", "") and "A" not in info.get("flags", ""):
                    activity.syn_count += 1
                if info.get("dport", 0) > 0:
                    activity.dst_ports.add(info["dport"])
            elif info["proto"] == "icmp":
                activity.icmp_count += 1

            # Check every line for aggressive patterns
            check_scan_detection(current_time)

    except KeyboardInterrupt:
        log("Monitor stopped by user.")
    except Exception as e:
        log(f"Monitor error: {e}")
    finally:
        if proc is not None:
            proc.terminate()

def run_health_check():
    """Periodic health check: verify pf is enabled, rules loaded."""
    try:
        result = subprocess.run(
            ["sudo", "-n", "pfctl", "-s", "info"],
            capture_output=True, text=True, timeout=10
        )
        if "Enabled" in result.stdout:
            log(f"pf Status: ENABLED | Banned hosts: {len(banned_hosts)}")
        else:
            log("WARNING: pf is DISABLED!")
            # Re-enable
            subprocess.run(["sudo", "-n", "pfctl", "-e"], capture_output=True, timeout=10)
            log("pf re-enabled.")
    except Exception as e:
        log(f"Health check error: {e}")

if __name__ == "__main__":
    print("=" * 60)
    print("  Net-Mask Sweep Detector — Big Mac Firewall")
    print("  Auto-bans port scanners and flooders")
    print("=" * 60)
    print()

    if "--health-check" in sys.argv:
        run_health_check()
        sys.exit(0)

    log("Sweep Detector starting...")
    log(f"Thresholds: SCAN={SCAN_THRESHOLD}ports, SYN={SYN_THRESHOLD}, ICMP={ICMP_THRESHOLD} (window={WINDOW_SECONDS}s)")

    # Handle graceful shutdown
    def handle_sigterm(sig, frame):
        log("Received SIGTERM, shutting down.")
        sys.exit(0)
    signal.signal(signal.SIGTERM, handle_sigterm)
    signal.signal(signal.SIGINT, handle_sigterm)

    tail_pflog()
