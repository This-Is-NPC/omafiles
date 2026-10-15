"""Own benchmark resources and preserve observations."""

import argparse
from datetime import datetime, timezone
import gzip
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shlex
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import time
import uuid
import zlib
from memory import cpu_core, memory_metrics, write as write_memory


ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ("1000", "10000", "100000", "images-2000", "1000000")
METRICS = ("frame_ms", "filter_ms", "wheel_ms", "peak_gui_rss_mib",
           "peak_tree_rss_mib", "settle_ms", "idle_cpu_percent", "thumbnail_ms", "preview_ms")
MEDIA = {"thumbnail_ms", "preview_ms"}
LABEL = "org.omafiles.benchmark"
REPEATS = 5


def stamp():
    return datetime.now(timezone.utc).isoformat()


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def execute(arguments, *, check=True, timeout=240, detached=False):
    result = subprocess.run(arguments, capture_output=True, text=True, timeout=timeout, start_new_session=detached)
    if check and result.returncode:
        raise RuntimeError(f"{shlex.join(map(str, arguments))}: {result.stderr.strip()}")
    return result


def parse_cpu_list(value):
    result = set()
    for part in value.split(","):
        bounds = part.split("-")
        if len(bounds) not in (1, 2) or any(not x.isdecimal() for x in bounds):
            raise ValueError("CPUs must be nonnegative IDs or inclusive ranges")
        first, last = int(bounds[0]), int(bounds[-1])
        if last < first:
            raise ValueError("CPU range is reversed")
        result.update(range(first, last + 1))
    return sorted(result)


def choose_cpus(requested=None):
    allowed = set(os.sched_getaffinity(0))
    if requested:
        chosen = parse_cpu_list(requested)
    else:
        chosen, cores = [], set()
        for cpu in sorted(allowed):
            key = cpu_core(cpu)
            if key not in cores:
                chosen.append(cpu)
                cores.add(key)
            if len(chosen) == 2:
                break
    if len(chosen) != 2 or not set(chosen) <= allowed:
        raise ValueError("A/B need exactly two distinct permitted CPUs; use --cores CPU,CPU")
    return {"A": {"cpus": 1, "cpuset": str(chosen[0]), "memory": "512m"},
            "B": {"cpus": 2, "cpuset": ",".join(map(str, chosen)), "memory": "1g"}}


def schedule(fixtures):
    for fixture in fixtures:
        for repeat in range(1, REPEATS + 1):
            for config in (("A", "B") if repeat % 2 else ("B", "A")):
                yield fixture, repeat, config


def wait_for_load(threshold, timeout, *, load=os.getloadavg, clock=time.monotonic,
                  sleep=time.sleep):
    started = clock()
    observations = []
    while True:
        sample = list(load())
        observations.append({"utc": stamp(), "loadavg": sample})
        elapsed = clock() - started
        if sample[0] < threshold:
            return {"wait_seconds": elapsed, "threshold": threshold, "samples": observations}
        if elapsed >= timeout:
            raise RuntimeError(f"Host load {sample[0]:.2f} did not fall below {threshold:g}; no batch started")
        sleep(min(5, timeout - elapsed))


class OwnedDocker:
    """Remove only names whose ownership labels match this invocation."""

    def __init__(self, call=execute, control=None):
        self.call = call
        self.token = uuid.uuid4().hex
        self.containers = []
        self.volumes = []
        self.serial = 0
        self.control = control
        self.record()

    def record(self):
        if self.control:
            temporary = self.control.with_suffix(".next")
            temporary.write_text(json.dumps({"token": self.token, "containers": self.containers, "volumes": self.volumes}))
            temporary.replace(self.control)

    def docker(self, *args, **kwargs):
        return self.call(["/usr/bin/docker", *map(str, args)], **kwargs)

    def volume(self):
        name = "omafiles-bench-" + self.token
        self.volumes.append(name)
        self.record()
        self.docker("volume", "create", "--label", f"{LABEL}={self.token}", name)
        return name

    def container(self, arguments):
        name = f"omafiles-bench-{self.token}-{self.serial}"
        self.serial += 1
        self.containers.append(name)
        self.record()
        self.docker("create", "--name", name, "--label", f"{LABEL}={self.token}", *arguments)
        return name

    def inspect(self, kind, name):
        done = self.docker(kind, "inspect", name, check=False, timeout=60)
        if done.returncode:
            missing = ("no such volume",) if kind == "volume" else ("no such object", "no such container")
            if name in done.stderr and any(message in done.stderr.lower() for message in missing):
                return None
            raise RuntimeError(done.stderr)
        records = json.loads(done.stdout)
        if len(records) != 1:
            raise RuntimeError("Unexpected Docker inspection result")
        return records[0]

    def remove(self, kind, name):
        record = self.inspect(kind, name)
        if record is None:
            return
        labels = record.get("Labels") if kind == "volume" else record.get("Config", {}).get("Labels")
        if (labels or {}).get(LABEL) != self.token:
            raise RuntimeError(f"Refusing to remove unowned {kind}: {name}")
        args = ("volume", "rm", name) if kind == "volume" else ("rm", "--force", name)
        self.docker(*args, timeout=300 if kind == "volume" else 60)

    def cleanup(self):
        errors = []
        for kind, names in (("container", self.containers), ("volume", self.volumes)):
            for name in reversed(names[:]):
                try:
                    self.remove(kind, name)
                    names.remove(name)
                    self.record()
                except (RuntimeError, subprocess.SubprocessError, ValueError) as error:
                    errors.append(str(error))
        return errors


def mount(source, target, kind="bind", writable=False):
    return ["--mount", f"type={kind},src={source},dst={target}" + ("" if writable else ",readonly")]


def container_arguments(config, staging, product, volume, image):
    args = ["--read-only", "--network=none", "--cap-drop=ALL", "--security-opt=no-new-privileges",
            "--user", f"{os.getuid()}:{os.getgid()}", "--tmpfs", "/tmp:rw,mode=1777",
            "--cpus=" + str(config["cpus"]), "--cpuset-cpus=" + config["cpuset"],
            "--memory=" + config["memory"], "--memory-swap=" + config["memory"]]
    args += mount(staging, "/bench") + mount(product, "/product")
    args += mount("/usr/lib", "/hostlib") + mount(volume, "/fixtures", "volume")
    return args + [image, "/usr/bin/python3", "/bench/session.py"]


def memory_cpu(configs):
    measured = set(parse_cpu_list(configs["B"]["cpuset"]))
    measured_cores = {cpu_core(cpu) for cpu in measured}
    spare = {cpu for cpu in os.sched_getaffinity(0) - measured
             if cpu_core(cpu) not in measured_cores}
    if not spare:
        raise RuntimeError("High-resolution memory measurement needs an allowed physical core outside A/B")
    return min(spare)


def measured_session(docker, name, directory, cpu, measured_cpus, *, memory_enabled=True):
    """Arm a separate host observer before the container launches its app."""
    observer = None
    with tempfile.TemporaryFile(mode="w+") as output, tempfile.TemporaryFile(mode="w+") as errors:
        attached = subprocess.Popen(["/usr/bin/docker", "start", "--attach", name],
                                    stdout=output, stderr=errors)
        try:
            deadline = time.monotonic() + 10
            state = docker.inspect("container", name)["State"]
            while not state.get("Running"):
                if attached.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("Container did not start its memory handshake")
                time.sleep(.01)
                state = docker.inspect("container", name)["State"]
            if memory_enabled:
                observer = subprocess.Popen([
                    "/usr/bin/python3", str(directory.parent / "memory.py"),
                    "--control", str(directory), "--container-pid", str(state["Pid"]),
                    "--cpu", str(cpu), "--measured-cpus", measured_cpus],
                    stdout=errors, stderr=errors)
            else:
                write_memory(directory / "armed.json", {"disabled": True})
            attached.wait(timeout=450)
            if observer:
                observer.wait(timeout=15)
                if observer.returncode:
                    try:
                        detail = json.loads((directory / "result.json").read_text()).get("error")
                    except (OSError, ValueError):
                        detail = None
                    raise RuntimeError("Independent memory observer failed: " +
                                       (str(detail) if detail else f"exit {observer.returncode}"))
            output.seek(0)
            errors.seek(0)
            return subprocess.CompletedProcess(attached.args, attached.returncode,
                                               output.read(), errors.read())
        finally:
            write_memory(directory / "stop.json", {})
            for process in (observer, attached):
                if process and process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()


def parse_session(text):
    def finite_number(text):
        number = float(text)
        if not math.isfinite(number):
            raise ValueError("Nonfinite numbers cannot be benchmark evidence")
        return number
    value = json.loads(text, parse_float=finite_number, parse_constant=finite_number)
    if not isinstance(value, dict):
        raise ValueError("Session output must be one JSON object")
    for metric in METRICS:
        number = value.get(metric)
        if number is not None and (isinstance(number, bool) or not isinstance(number, (float, int))
                                   or not math.isfinite(number) or number < 0):
            raise ValueError(f"Invalid session metric: {metric}")
    for name in ("metric_reasons", "phases", "cgroup_before", "cgroup_after", "docker_state", "host_load", "memory"):
        if name in value and not isinstance(value[name], dict):
            raise ValueError(f"Invalid session evidence: {name}")
    if any(not isinstance(reason, str) for reason in value.get("metric_reasons", {}).values()):
        raise ValueError("Metric reasons must be strings")
    return value


def normalize_evidence(run, identity):
    reason = run.get("error") or run.get("infrastructure_error")
    application = run.get("application") or {}
    if (application.get("viewport_width") != 1180 or application.get("viewport_height") != 720
            or application not in run.get("events", [])):
        reason = "Run lacks the required 1180×720 application viewport"
    counts = run.get("semantic_counts", {})
    files = 2000 if run.get("fixture") == "images-2000" else int(run["fixture"])
    parent_row = 1 if run.get("app") == "omafiles" else 0
    expected = {"fixture_files": files, "listing_expected": files + parent_row, "filter_expected": 1 + parent_row}
    listing = run.get("phases", {}).get("listing", {})
    if (counts != expected or listing.get("status") != "complete"
            or listing.get("evidence", {}).get("visible_count") != expected["listing_expected"]):
        reason = "Run lacks verified completed listing evidence"
    if run.get("process_returncode_before_cleanup"):
        reason = "Application process exited before completing the measurement workflow"
    state = run.get("docker_state", {})
    before = run.get("cgroup_before", {}).get("memory.events") or {}
    after = run.get("cgroup_after", {}).get("memory.events") or {}
    if (state.get("OOMKilled") or state.get("ExitCode", 0) or run.get("oom_kill_delta", 0)
            or int(after.get("oom_kill", 0)) > int(before.get("oom_kill", 0))):
        reason = "Run records a process failure or OOM kill"
    config = identity.get("configs", {}).get(run.get("config"))
    cgroup = run.get("cgroup_after")
    if config and not cgroup:
        reason = "Run lacks recorded cgroup limits"
    elif config:
        expected_memory = {"512m": 512 * 1024**2, "1g": 1024**3}[config["memory"]]
        quota, period = map(int, cgroup["cpu.max"].split())
        if (int(cgroup["memory.max"]) != expected_memory or int(cgroup["memory.swap.max"]) != 0
                or quota != config["cpus"] * period
                or parse_cpu_list(cgroup["cpuset.cpus.effective"]) != parse_cpu_list(config["cpuset"])):
            reason = "Recorded cgroup limits do not match the benchmark configuration"
    load = run.get("host_load", {})
    samples = load.get("samples", [])
    threshold = identity.get("load_threshold")
    if threshold is not None:
        if not samples:
            reason = "Run lacks recorded accepted host load"
        elif (load.get("threshold") != threshold or len(samples[-1]["loadavg"]) != 3
              or any(type(value) not in (int, float) or not math.isfinite(value) or value < 0
                     for value in samples[-1]["loadavg"])
              or samples[-1]["loadavg"][0] >= threshold):
            reason = "Recorded accepted host load does not satisfy the benchmark threshold"
    if (type(run.get("latency_only", False)) is not bool
            or run.get("latency_only", False) != identity.get("latency_only", False)):
        reason = "Session observer mode contradicts document identity"
    if reason:
        run.update({metric: None for metric in METRICS})
        run["metric_reasons"] = {metric: reason for metric in METRICS}
    for metric in METRICS:
        name = metric.removesuffix("_ms")
        if name not in ("wheel", "filter", "thumbnail", "preview") or run.get(metric) is None:
            continue
        phase = run.get("phases", {}).get(name, {})
        evidence = phase.get("evidence", {})
        expected_count = expected["filter_expected" if name == "filter" else "listing_expected"]
        if (phase.get("status") != "complete" or evidence.get("event") != "complete"
                or evidence.get("operation") != name or evidence.get("ms") != run[metric]
                or ("deadline_seconds" in phase and phase["elapsed_ms"] > phase["deadline_seconds"] * 1000)
                or (name == "filter" and run.get("phases", {}).get("open_filter", {}).get("status") != "complete")
                or (name in ("filter", "wheel") and evidence.get("visible_count") != expected_count)):
            run[metric] = None
            run.setdefault("metric_reasons", {})[metric] = phase.get("reason", "Recorded observation phase lacks consistent completion evidence")
    memory = run.get("memory", {})
    if not identity.get("latency_only", False):
        if (memory.get("observer_cpu") != identity.get("memory_observer_cpu")
                or memory.get("measured_cpus") != parse_cpu_list(identity["configs"]["B"]["cpuset"])):
            for metric in ("peak_gui_rss_mib", "peak_tree_rss_mib"):
                run[metric] = None
                run.setdefault("metric_reasons", {})[metric] = "Memory observer placement contradicts run identity"
        if memory.get("stop_elapsed_ms", -1) < max((s["elapsed_ms"] for s in run.get("samples", [])), default=0):
            for metric in ("peak_gui_rss_mib", "peak_tree_rss_mib"):
                run[metric] = None
                run.setdefault("metric_reasons", {})[metric] = "Memory observer stopped before the session's last sample"
    validate_observations(run)
    return run


def normalize_run(run, identity):
    """Rank latency from the unobserved session and RSS from its independent replay."""
    if identity.get("latency_only", False):
        return normalize_evidence(run, identity)
    cached = {metric: run.get(metric) for metric in METRICS if metric.endswith("rss_mib")}
    run = normalize_evidence(run, dict(identity, latency_only=True))
    memory = run.get("memory_session")
    if not isinstance(memory, dict):
        raise ValueError("Separate memory session evidence is required")
    memory = parse_session(json.dumps(memory))
    fields = ("app", "config", "fixture", "repeat")
    if any(memory.get(field) != run.get(field) for field in fields):
        raise ValueError("Memory session does not match its latency session")
    memory = normalize_evidence(memory, dict(identity, latency_only=False))
    run["memory_session"] = memory
    for metric, old in cached.items():
        value = memory.get(metric)
        if old is not None and value is not None and not math.isclose(old, value, rel_tol=1e-12, abs_tol=1e-12):
            value = None
            reason = "Cached RSS contradicts the separate memory session"
        else:
            reason = memory.get("metric_reasons", {}).get(metric, "Independent memory observation unavailable")
        run[metric] = value
        if value is None:
            run.setdefault("metric_reasons", {})[metric] = reason
        else:
            run.setdefault("metric_reasons", {}).pop(metric, None)
    return run


def validate_observations(run):
    events, samples = run.get("events", []), run.get("samples", [])
    frames = [event.get("ms") for event in events if event.get("event") == "first_frame"]
    expected = {"frame_ms": frames[0] if frames else None}
    try:
        expected.update(memory_metrics(run.get("memory", {})))
    except (ValueError, KeyError, TypeError):
        expected.update(peak_gui_rss_mib=None, peak_tree_rss_mib=None)
    settled = next((s for s in samples if s.get("elapsed_ms") == run.get("settle_ms")), None)
    if settled:
        quiet_start = settled["elapsed_ms"]
        for sample in reversed([s for s in samples if s["elapsed_ms"] < settled["elapsed_ms"]]):
            if sample["ticks"] != settled["ticks"]:
                break
            quiet_start = sample["elapsed_ms"]
        if settled["elapsed_ms"] - quiet_start < 500:
            settled = None
    if not settled:
        expected.update(settle_ms=None, idle_cpu_percent=None)
    else:
        end = next((s for s in samples if s["elapsed_ms"] >= settled["elapsed_ms"] + 1000), None)
        expected["idle_cpu_percent"] = ((end["ticks"] - settled["ticks"]) / os.sysconf("SC_CLK_TCK")
            / ((end["elapsed_ms"] - settled["elapsed_ms"])/1000) * 100) if end else None
    for metric in ("filter_ms", "wheel_ms", "thumbnail_ms", "preview_ms"):
        evidence = run.get("phases", {}).get(metric.removesuffix("_ms"), {}).get("evidence")
        if evidence not in events:
            expected[metric] = None
    for metric, value in expected.items():
        actual = run.get(metric)
        if actual is not None and (value is None or not math.isclose(actual, value, rel_tol=1e-12, abs_tol=1e-12)):
            run[metric] = None
            run.setdefault("metric_reasons", {})[metric] = "Metric contradicts or lacks its recorded observer/process samples"


def summarize(runs):
    groups = {}
    for run in runs:
        key = (run["config"], run["fixture"])
        groups.setdefault(key, []).append(run)
    summaries = {}
    for (config, fixture), group in groups.items():
        metrics = {}
        for metric in METRICS:
            if fixture != "images-2000" and metric in MEDIA:
                continue
            values = [r[metric] for r in group if r.get(metric) is not None]
            subsequent = [r[metric] for r in group[1:] if r.get(metric) is not None]
            reasons = sorted({r.get("metric_reasons", {}).get(metric, r.get("error") or "unavailable")
                              for r in group if r.get(metric) is None})
            metrics[metric] = {"n": len(values), "attempted": len(group), "reasons": reasons,
                               "median": statistics.median(values) if values else None,
                               "min": min(values) if values else None, "max": max(values) if values else None,
                               "spread": max(values) - min(values) if values else None,
                               "first_launch": group[0].get(metric),
                               "subsequent_median": statistics.median(subsequent) if subsequent else None}
        summaries[f"{config}/{fixture}"] = metrics
    return summaries


def scaling(summary):
    result = {}
    for case, metrics in summary.items():
        if not case.startswith("A/"):
            continue
        fixture = case[2:]
        other = summary.get("B/" + fixture, {})
        result[fixture] = {}
        for metric, left in metrics.items():
            right = other.get(metric)
            ready = right and left["n"] == right["n"] == REPEATS
            a, b = left["median"], right["median"] if right else None
            if metric == "idle_cpu_percent":
                value = {"delta_percentage_points": b - a if ready else None}
            else:
                value = {"ratio_a_over_b": a / b if ready and b else None,
                         "kind": "memory ratio" if metric.endswith("rss_mib") else "latency ratio"}
            result[fixture][metric] = value
    return result


def table(document):
    lines = [f"# {document['app']} benchmark", "", f"Status: {document['status']}", "",
             "A → B changes CPU quota AND memory. Ratios do not isolate CPU scaling.", "",
             "| Case | Metric | n | Median | Min–max | Spread | A/B or idle Δ pp |",
             "|---|---|---:|---:|---|---:|---:|"]
    lines.insert(4, "Latency and RSS use separate fresh sessions; memory-replay timings are diagnostic only.\n")
    if document.get("purpose") == "latency-only":
        lines.insert(4, "Diagnostic latency-only sessions: independent RSS is unavailable.\n")
    def number(value):
        return "—" if value is None else f"{value:.3f}"
    for case, metrics in document["summary"].items():
        for metric, value in metrics.items():
            ratio = document["a_to_b"].get(case[2:], {}).get(metric, {})
            ratio = ratio.get("delta_percentage_points", ratio.get("ratio_a_over_b"))
            lines.append(f"| {case} | {metric} | {value['n']}/{value['attempted']} | "
                         f"{number(value['median'])} | {number(value['min'])}–{number(value['max'])} | "
                         f"{number(value['spread'])} | {number(ratio)} |")
    for run in document["runs"]:
        for metric, reason in run.get("metric_reasons", {}).items():
            if metric in MEDIA and run["fixture"] != "images-2000":
                continue
            lines.append(f"\n{run['config']}/{run['fixture']} run {run['repeat']} {metric}: {reason}.")
    if document.get("error"):
        lines.append("\nError: " + document["error"])
    return "\n".join(lines) + "\n"


def output_path(value):
    path = Path(value).expanduser()
    if path.is_symlink():
        raise ValueError("Benchmark outputs cannot be symbolic links")
    if path.exists() and not path.is_file():
        raise ValueError("Benchmark outputs must be regular files")
    if path.exists() and path.stat().st_nlink != 1:
        raise ValueError("Benchmark outputs cannot have hard links")
    path = path.resolve()
    if not (path.is_relative_to(ROOT) or path.is_relative_to(Path("/tmp"))):
        raise ValueError("Benchmark outputs must be inside the repository or /tmp")
    if not path.parent.is_dir():
        raise ValueError(f"Output parent does not exist: {path.parent}")
    return path


def load_json(path):
    path = Path(path)
    data = path.read_bytes()
    if path.suffix == ".gz":
        try:
            data = gzip.decompress(data)
        except (gzip.BadGzipFile, EOFError, zlib.error) as error:
            raise ValueError(f"Invalid gzip benchmark report: {path}") from error
    return json.loads(data)


def save(document, path):
    path = output_path(path)
    compressed = path.name.endswith(".json.gz")
    if path.suffix != ".json" and not compressed:
        raise ValueError("Benchmark JSON output must have a .json or .json.gz suffix")
    stem = path.with_suffix("") if compressed else path
    markdown = output_path(stem.with_suffix(".md"))
    data = (json.dumps(document, indent=2, allow_nan=False) + "\n").encode("utf-8")
    if compressed:
        data = gzip.compress(data, compresslevel=6, mtime=0)
    path.write_bytes(data)
    markdown.write_text(table(document))


def runtime_identity(image, docker, configs, staging, threshold):
    identity = json.loads(docker.docker("image", "inspect", image).stdout)[0]
    libraries = ("libQt6Core.so.6", "libQt6Quick.so.6", "libQt6Qml.so.6", "libc.so.6")
    return {"method": 3, "image": identity["Id"], "architecture": platform.machine(),
            "memory_observer_cpu": memory_cpu(configs),
            "kernel": platform.release(), "cpu_model": next(
                line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
                if line.startswith("model name")), "configs": configs, "load_threshold": threshold,
            "runtime_sha256": {name: digest(Path("/usr/lib") / name) for name in libraries},
            "harness_sha256": {name: digest(staging / name) for name in
                               ("probe.cpp", "session.py", "fixtures.py", "memory.py")}}


def prepare(staging, build):
    (staging / "bin").mkdir()
    shim = staging / "bin/qs"
    shim.write_text('#!/bin/sh\nexport LD_PRELOAD="$BENCH_PROBE"\nexec /usr/bin/qs "$@"\n')
    shim.chmod(0o755)
    for path in Path(__file__).parent.glob("*.py"):
        if path.name != "runner.py":
            shutil.copy2(path, staging / path.name)
    shutil.copy2(Path(__file__).parent / "probe.cpp", staging / "probe.cpp")
    flags = shlex.split(execute(["/usr/bin/pkg-config", "--cflags", "--libs", "Qt6Quick", "Qt6Test"]).stdout)
    execute(["/usr/bin/c++", "-shared", "-fPIC", "-std=c++17", str(staging / "probe.cpp"),
             "-o", str(staging / "probe.so"), *flags], timeout=180)
    if build is None:
        execute([str(ROOT / "scripts/build.sh")], timeout=1200)
        build = ROOT / "build/bin"
    product = staging / "product"
    product.mkdir()
    for name in ("omafiles", "omafiles-studio", "omafiles-preview", "omafiles-archive", "omafiles-devices"):
        shutil.copy2(build / name, product / name)
    return product


def required_metrics(latency_only=False):
    return tuple(metric for metric in METRICS if metric not in MEDIA
                 and (not latency_only or not metric.endswith("rss_mib")))


def collect_session(docker, staging, product, volume, args, document, configs,
                    fixture, repeat, config, latency_only):
    mode = "latency" if latency_only else "memory"
    print(f"{config}/{fixture} run {repeat}/{REPEATS} {mode}: waiting for host load", file=sys.stderr, flush=True)
    load = wait_for_load(document["identity"]["load_threshold"], args.load_timeout)
    directory = staging / f"{mode}-{fixture}-{repeat}-{config}"
    directory.mkdir()
    command = container_arguments(configs[config], staging, product, volume, args.image)
    command = mount(directory, "/memory", writable=True) + command
    command += ["--app", document["app"], "--fixture", fixture, "--probe", "/bench/probe.so",
                "--memory-control", "/memory"]
    if latency_only:
        command.append("--latency-only")
    name = docker.container(command)
    result = measured_session(docker, name, directory, document["identity"]["memory_observer_cpu"],
                              configs["B"]["cpuset"], memory_enabled=not latency_only)
    state = docker.inspect("container", name)["State"]
    try:
        run = parse_session(result.stdout)
    except (ValueError, TypeError) as error:
        run = {"error": str(error), "metric_reasons": {m: "invalid or missing session output" for m in METRICS}}
    run.update(config=config, fixture=fixture, repeat=repeat, host_load=load,
               host_load_after=list(os.getloadavg()), docker_state=state, docker_stderr=result.stderr[-6000:])
    if state.get("OOMKilled") or state["ExitCode"] or result.returncode or run.get("oom_kill_delta", 0):
        run["error"] = (f"Container failed: exit {state['ExitCode']}; OOMKilled={state.get('OOMKilled')}; "
                        f"child OOM kills={run.get('oom_kill_delta', 0)}")
    run = normalize_evidence(run, dict(document["identity"], latency_only=latency_only))
    docker.remove("container", name)
    docker.containers.remove(name)
    docker.record()
    shutil.rmtree(directory)
    return run


def measure(args):
    latency_only = getattr(args, "latency_only", False)
    configs = choose_cpus(args.cores)
    threshold = args.load_threshold if args.load_threshold is not None else max(1, len(os.sched_getaffinity(0)) / 2)
    if not math.isfinite(threshold) or threshold <= 0 or not math.isfinite(args.load_timeout) or args.load_timeout < 0:
        raise ValueError("Load threshold must be positive and timeout nonnegative")
    document = {"schema": 1, "app": "omafiles", "started_utc": stamp(), "runs": [], "status": "incomplete", "error": None}
    if latency_only:
        document["purpose"] = "latency-only"
    docker = OwnedDocker(control=args.control)
    document["owner"] = docker.token
    document.update(summary={}, a_to_b={})
    save(document, args.output)
    try:
        with tempfile.TemporaryDirectory(prefix="omafiles-bench-", dir=args.control.parent if args.control else "/tmp") as temporary:
            staging = Path(temporary)
            staging.chmod(0o755)
            product = prepare(staging, args.build)
            document["identity"] = runtime_identity(args.image, docker, configs, staging, threshold)
            if latency_only:
                document["identity"]["latency_only"] = True
            document["product_sha256"] = {p.name: digest(p) for p in sorted(product.iterdir())}
            volume = docker.volume()
            populate = ["--read-only", "--network=none", "--cap-drop=ALL", "--security-opt=no-new-privileges",
                        "--tmpfs", "/tmp:rw,mode=1777"]
            populate += mount(staging, "/bench") + mount(volume, "/fixtures", "volume", True)
            populate += [args.image, "/usr/bin/python3", "/bench/fixtures.py"]
            for fixture in args.fixtures:
                populate += ["--fixture", fixture]
            helper = docker.container(populate)
            document["fixtures"] = json.loads(docker.docker("start", "--attach", helper, timeout=600).stdout)
            if docker.inspect("container", helper)["State"]["ExitCode"]:
                raise RuntimeError("Fixture helper failed")
            docker.remove("container", helper)
            docker.containers.remove(helper)
            docker.record()
            for fixture, repeat, config in schedule(args.fixtures):
                sessions = {}
                document["pending_sessions"] = {"config": config, "fixture": fixture, "repeat": repeat, "sessions": {}}
                modes = (True,) if latency_only else ((True, False) if repeat % 2 else (False, True))
                for unobserved in modes:
                    sessions[unobserved] = collect_session(docker, staging, product, volume, args, document,
                                                          configs, fixture, repeat, config, unobserved)
                    document["pending_sessions"]["sessions"]["latency" if unobserved else "memory"] = sessions[unobserved]
                    save(document, args.output)
                observation = sessions[True]
                if not latency_only:
                    observation["memory_session"] = sessions[False]
                observation = normalize_run(observation, document["identity"])
                document["runs"].append(observation)
                del document["pending_sessions"]
                document["summary"] = summarize(document["runs"])
                document["a_to_b"] = scaling(document["summary"])
                save(document, args.output)
    except (RuntimeError, ValueError, OSError, subprocess.SubprocessError, KeyboardInterrupt) as error:
        document["error"] = str(error) or "interrupted"
    finally:
        for number in (signal.SIGINT, signal.SIGTERM):
            signal.signal(number, signal.SIG_IGN)
        document["cleanup_errors"] = docker.cleanup()
    document["finished_utc"] = stamp()
    document["summary"] = summarize(document["runs"])
    document["a_to_b"] = scaling(document["summary"])
    complete = len(document["runs"]) == len(args.fixtures) * REPEATS * 2
    complete = complete and all(run.get(metric) is not None for run in document["runs"]
                                for metric in required_metrics(latency_only))
    document["status"] = "complete" if complete and not document["error"] and not document["cleanup_errors"] else "incomplete"
    save(document, args.output)
    print(table(document), end="")
    if document["status"] != "complete" or any(r.get("error") for r in document["runs"]):
        return 2
    if any(run.get(metric) is None for run in document["runs"] for metric in required_metrics(latency_only)):
        return 2
    return 0


def cleanup_control(path):
    data = json.loads(Path(path).read_text())
    if len(data["token"]) != 32 or any(c not in "0123456789abcdef" for c in data["token"]):
        raise ValueError("Invalid benchmark ownership token")
    docker = OwnedDocker()
    docker.token = data["token"]
    for kind in ("containers", "volumes"):
        names = data[kind]
        if any(not name.startswith("omafiles-bench-" + docker.token) for name in names):
            raise ValueError("Invalid benchmark resource name")
        setattr(docker, kind, names)
    errors = docker.cleanup()
    if errors:
        raise RuntimeError("; ".join(errors))


def supervise(argv, output):
    directory = Path(tempfile.mkdtemp(prefix="omafiles-bench-control-", dir="/tmp"))
    control = directory / "resources.json"
    command = ["/usr/bin/python3", str(ROOT / "scripts/test-environment.py"),
               str(ROOT / "scripts/bench"), *argv, "--control", str(control), "--output", str(output)]
    process = subprocess.Popen(command, start_new_session=True)
    status, errors = 2, []
    try:
        status = process.wait()
    except KeyboardInterrupt:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
        status = 130
    finally:
        for number in (signal.SIGINT, signal.SIGTERM):
            signal.signal(number, signal.SIG_IGN)
        if control.exists():
            cleanup = execute(["/usr/bin/python3", str(ROOT / "scripts/test-environment.py"),
                               str(ROOT / "scripts/bench"), "--cleanup-control", str(control)],
                              check=False, timeout=600, detached=True)
            if cleanup.returncode:
                errors.append(cleanup.stderr)
                print(f"bench cleanup failed; ownership journal retained at {control}: " + cleanup.stderr,
                      file=sys.stderr)
                status = 2
        if output.is_file():
            document = load_json(output)
            document["cleanup_errors"] = errors
            if status not in (0, 1):
                document.update(status="incomplete", error=f"Benchmark process exited {status}")
            if errors:
                document["cleanup_journal"] = str(control)
            save(document, output)
        if not errors:
            shutil.rmtree(directory)
    return status


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, help="existing build/bin; otherwise build the repository")
    parser.add_argument("--image", default="omafiles-t02:local", help="already installed Linux runtime image; never pulled or built")
    parser.add_argument("--cores", help="two permitted CPU IDs; defaults to distinct physical cores")
    parser.add_argument("--load-threshold", type=float, help="strict upper bound for 1-minute host load; default allowed logical CPUs / 2")
    parser.add_argument("--load-timeout", type=float, default=600, help="seconds to wait before refusing a batch")
    parser.add_argument("--fixtures", nargs="+", choices=FIXTURES, default=list(FIXTURES))
    parser.add_argument("--output", help="JSON path; readable .md table is written beside it")
    parser.add_argument("--latency-only", action="store_true",
                        help="disable high-rate memory observation for latency diagnostics; no RSS")
    parser.add_argument("--control", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--cleanup-control", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    try:
        if args.cleanup_control:
            if not os.environ.get("OMAFILES_TEST_ENV"):
                raise ValueError("Cleanup requires the isolated command environment")
            cleanup_control(args.cleanup_control)
            return 0
        args.output = output_path(args.output or f"/tmp/omafiles-bench-{uuid.uuid4().hex}.json.gz")
        args.fixtures = list(dict.fromkeys(args.fixtures))
        def interrupted(_number, _frame):
            raise KeyboardInterrupt
        for number in (signal.SIGINT, signal.SIGTERM):
            signal.signal(number, interrupted)
        if not os.environ.get("OMAFILES_TEST_ENV"):
            return supervise(argv if argv is not None else sys.argv[1:], args.output)
        return measure(args)
    except (RuntimeError, ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"bench: {error}", file=sys.stderr)
        return 2
