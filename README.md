# OS-Jackfruit Container Runtime with Kernel Monitoring

---

## 1. Team Information

* Name: Ishanika Vinuthan

* SRN: PES2UG24CS911

* Name: Nishkaa V

* SRN: PES2UG24CS907

---

## 2. Build, Load, and Run Instructions

### Step 1: Navigate to project directory

```bash
cd ~/OS-Jackfruit/boilerplate
```

---

### Step 2: Build the project

```bash
make
```

---

### Step 3: Load the kernel module

```bash
sudo insmod monitor.ko
```

---

### Step 4: Verify device creation

```bash
ls -l /dev/container_monitor
```

---

### Step 5: Start supervisor

```bash
sudo ./engine supervisor ./rootfs-base
```

---

### Step 6: Create container root filesystems

Open a **second terminal** and run:

```bash
cd ~/OS-Jackfruit/boilerplate

sudo rm -rf rootfs-alpha rootfs-beta rootfs-mem

cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

---

### Step 7: Copy workload programs

```bash
cp ./cpu_hog ./rootfs-alpha/
cp ./cpu_hog ./rootfs-beta/
```

---

### Step 8: Start containers

```bash
sudo ./engine start alpha ./rootfs-alpha "/cpu_hog 5"
sudo ./engine start beta ./rootfs-beta "/cpu_hog 5"
```

---

### Step 9: Check running containers

```bash
sleep 2
sudo ./engine ps
```

---

### Step 10: View logs

```bash
sudo ./engine logs alpha
```

---

### Step 11: Memory stress test

```bash
cp -a ./rootfs-base ./rootfs-mem
cp ./memory_hog ./rootfs-mem/

sudo ./engine start mem ./rootfs-mem "/memory_hog 8 200" --soft-mib 16 --hard-mib 32
```

---

### Step 12: Observe system behavior

```bash
sleep 5
sudo ./engine ps
sudo dmesg | tail -n 30
sudo ./engine logs mem
```

---

### Step 13: Stop containers

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
```

---

### Step 14: Unload kernel module

```bash
sudo rmmod monitor
```

---

## 3. Demo with Screenshots

Screenshots will be added for the following:

1. Multi-container supervision
<img width="827" height="286" alt="1" src="https://github.com/user-attachments/assets/3c19dd68-3f76-4ca5-96d3-86d8960dd425" />

2. Metadata tracking using `ps`
<img width="690" height="114" alt="2" src="https://github.com/user-attachments/assets/cf68ad54-80be-4751-ba90-44d79e9ed456" />

3. Logging system output
<img width="729" height="523" alt="3" src="https://github.com/user-attachments/assets/c0a8199f-9a8d-47e6-8dab-ea12800a1ae0" />

<img width="534" height="299" alt="4" src="https://github.com/user-attachments/assets/38971baf-7b0d-4d8a-84d7-a8e69ef4e97a" />

4. CLI interaction with supervisor
<img width="834" height="109" alt="5" src="https://github.com/user-attachments/assets/9a8bbe76-8471-43a7-ba2c-8429b7d1d2f1" />

5. Soft memory limit warning
<img width="833" height="45" alt="8" src="https://github.com/user-attachments/assets/521e007c-84ee-4f25-950f-1398fe86a6b3" />

6. Hard memory limit enforcement
<img width="828" height="44" alt="9" src="https://github.com/user-attachments/assets/7a9c5fee-842f-4ce1-83f1-371e699281ad" />

7. Scheduling experiment results
<img width="840" height="618" alt="10" src="https://github.com/user-attachments/assets/1df9d184-24fe-4d10-b5f7-84757327480e" />

8. Clean system shutdown
<img width="821" height="93" alt="11" src="https://github.com/user-attachments/assets/e3e93cf2-5479-478b-a0e0-c2e1d5e4bd19" />
<img width="832" height="100" alt="image" src="https://github.com/user-attachments/assets/62f6c8ee-a557-4ded-a99b-20be55855939" />


---

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

The container runtime achieves isolation primarily through **filesystem separation using `chroot`** and controlled process execution. When a container is started, the process is confined to a specific root filesystem, preventing it from accessing files outside its designated directory tree.

At the kernel level, stronger isolation in modern systems is typically implemented using **namespaces**:

* **PID namespace** isolates process IDs so that processes inside a container see their own process tree.
* **UTS namespace** isolates hostname and domain name.
* **Mount namespace** isolates filesystem mount points.

In this project, `chroot` provides a lightweight approximation of filesystem isolation but does not fully replicate namespace behavior. The host kernel is still shared across all containers, meaning:

* All containers use the same kernel instance
* System calls are handled by the same kernel
* Resources like CPU and memory are globally managed

Thus, while filesystem visibility is restricted, **true kernel-level isolation is not fully achieved**, highlighting the difference between lightweight containers and full virtualization.

---

### 4.2 Supervisor and Process Lifecycle

A long-running **supervisor process** is used to manage all containers. This design reflects how real container runtimes (like Docker) operate.

The supervisor is responsible for:

* Creating container processes using `fork()` and `exec()`
* Maintaining metadata such as container ID, PID, and state
* Monitoring lifecycle events
* Handling cleanup when containers terminate

Each container runs as a **child process** of the supervisor. This parent-child relationship is crucial because:

* The parent can track and manage all child processes
* The parent is responsible for **reaping terminated processes** using `wait()` to prevent zombie processes
* Signals (e.g., termination signals) can be propagated from supervisor to containers

Without a supervisor, there would be no centralized control, making it difficult to:

* Track running containers
* Clean up resources
* Maintain system stability

---

### 4.3 IPC, Threads, and Synchronization

The system uses multiple **inter-process communication (IPC) mechanisms** to coordinate between components:

1. **ioctl-based communication**

   * Used between user-space (`engine`) and kernel-space (`monitor`)
   * Enables registration and monitoring of container processes

2. **Shared logging pipeline (bounded buffer)**

   * Implements a producer-consumer model using threads

The bounded buffer introduces potential race conditions:

* Multiple producer threads writing logs simultaneously
* Consumer thread reading logs while producers modify the buffer

To handle this, synchronization mechanisms are used:

* **Mutex locks** ensure mutual exclusion when accessing shared buffers
* **Condition variables** coordinate producer-consumer behavior (e.g., wait when buffer is full/empty)

These choices are justified because:

* Mutexes prevent data corruption
* Condition variables avoid busy waiting, improving efficiency

Without proper synchronization, the system could experience:

* Data inconsistency
* Lost log entries
* Crashes due to concurrent access

---

### 4.4 Memory Management and Enforcement

Memory monitoring is based on **Resident Set Size (RSS)**, which measures the actual physical memory used by a process.

However, RSS does not include:

* Swapped-out memory
* Shared memory that may be counted differently across processes
* Kernel memory usage

The system implements two types of limits:

* **Soft limit**

  * Acts as a warning threshold
  * Logs are generated when exceeded
  * Does not terminate the process

* **Hard limit**

  * Enforced strictly
  * Process is terminated when exceeded

This distinction allows flexibility:

* Soft limits help observe behavior without immediate disruption
* Hard limits enforce strict resource control

Enforcement is implemented in **kernel space** because:

* The kernel has direct access to process memory statistics
* It can reliably enforce limits regardless of user-space behavior
* User-space alone cannot prevent a malicious or runaway process from exceeding limits

Thus, kernel-level enforcement ensures **robust and secure resource control**.

---

### 4.5 Scheduling Behavior

Scheduling experiments were conducted using CPU-intensive workloads (`cpu_hog`) running in multiple containers.

Observed behavior:

* CPU time was shared among containers
* Processes were interleaved using time-slicing
* No single process monopolized CPU resources

This reflects the goals of the Linux scheduler:

* **Fairness**

  * Each process gets a fair share of CPU time
* **Responsiveness**

  * Interactive processes remain responsive even under load
* **Throughput**

  * Overall system work is maximized

The scheduler dynamically adjusts execution based on system load, ensuring balanced resource allocation.

When multiple CPU-bound processes run simultaneously:

* They compete for CPU time
* The scheduler distributes time slices to maintain fairness

These results demonstrate how Linux scheduling policies maintain system stability and performance under contention.

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation

* Choice: Minimal isolation using chroot
* Tradeoff: Simpler implementation but weaker isolation compared to full namespaces

### Supervisor Architecture

* Choice: Centralized process manager
* Tradeoff: Easier control vs single point of failure

### IPC and Logging

* Choice: Bounded buffer with threads
* Tradeoff: Efficient logging but added synchronization complexity

### Kernel Monitor

* Choice: Loadable Kernel Module (LKM)
* Tradeoff: Deep system access vs higher complexity and debugging difficulty

### Scheduling Experiments

* Choice: CPU-bound workloads
* Tradeoff: Clear behavior observation but limited real-world variability

---

## 6. Scheduler Experiment Results

Experiments were conducted using CPU-intensive workloads (`cpu_hog`) across multiple containers.

Observations:

* CPU time was shared between containers
* Linux scheduler dynamically balanced execution
* Under load, processes experienced time-slicing
* Memory pressure influenced scheduling behavior indirectly

These results demonstrate how Linux fairly distributes CPU resources among competing processes.

---

## 7. Conclusion

This project successfully implements a lightweight container runtime with integrated kernel-level monitoring. It highlights key operating system concepts such as process isolation, scheduling, memory management, and kernel-user space interaction.

---

