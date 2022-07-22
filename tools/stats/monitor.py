#!/usr/bin/env python3
import datetime
import json
import signal
import subprocess
import sys
import time
from typing import Any, Dict, List


def add_package_to_path(package_name: str) -> None:
    location = subprocess.run(
        [sys.executable, "-m", "pip", "show", package_name],
        stderr=subprocess.PIPE,
        stdout=subprocess.PIPE,
    )
    for s in location.stdout.decode("utf-8").splitlines():
        if "Location:" in s:
            sys.path.append(s.split(" ")[1])


if __name__ == "__main__":
    # add_package_to_path("psutil")
    # add_package_to_path("pynvml")
    import psutil  # type: ignore[import]
    import pynvml  # type: ignore[import]

    def get_processes_running_python_tests() -> List[Any]:
        python_processes = []
        for process in psutil.process_iter():
            try:
                if "python" in process.name() and process.cmdline():
                    python_processes.append(process)
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                # access denied or the process died
                pass
        return python_processes

    def get_per_process_cpu_info() -> List[Dict[str, Any]]:
        processes = get_processes_running_python_tests()
        per_process_info = []
        for p in processes:
            info = {
                "pid": p.pid,
                "cmd": " ".join(p.cmdline()),
                "cpu_percent": p.cpu_percent(),
                "rss_memory": p.memory_info().rss,
                "uss_memory": p.memory_full_info().uss,
            }
            if "pss" in p.memory_full_info():
                # only availiable in linux
                info["pss_memory"] = p.memory_full_info().pss
            per_process_info.append(info)
        return per_process_info

    def get_per_process_gpu_info(handle: Any) -> List[Dict[str, Any]]:
        processes = pynvml.nvmlDeviceGetComputeRunningProcesses(handle)
        per_process_info = []
        for p in processes:
            info = {"pid": p.pid, "gpu_memory": p.usedGpuMemory}
            per_process_info.append(info)
        return per_process_info

    handle = None
    try:
        pynvml.nvmlInit()
        handle = pynvml.nvmlDeviceGetHandleByIndex(0)
    except pynvml.NVMLError:
        # no pynvml avaliable, probably because not cuda
        pass

    kill_now = False

    def exit_gracefully(*args: Any) -> None:
        global kill_now
        kill_now = True

    signal.signal(signal.SIGINT, exit_gracefully)
    signal.signal(signal.SIGTERM, exit_gracefully)

    while not kill_now:
        try:
            stats = {
                "time": datetime.datetime.utcnow().isoformat("T") + "Z",
                "total_cpu_percent": psutil.cpu_percent(),
                "per_process_cpu_info": get_per_process_cpu_info(),
            }
            if handle is not None:
                stats["per_process_gpu_info"] = get_per_process_gpu_info(handle)
                stats["total_gpu_utilizaiton"] = pynvml.nvmlDeviceGetUtilizationRates(
                    handle
                ).gpu
        except Exception as e:
            stats = {
                "time": datetime.datetime.utcnow().isoformat("T") + "Z",
                "error": str(e),
            }
        finally:
            print(json.dumps(stats))
            time.sleep(1)
