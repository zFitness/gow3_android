//! gport: nixport 宿主服务总线——应用内文件/目录选择（`nxp-port` 客户端）。
//!
//! 契约见 my-docs/移植规范/文件选择.md 与 宿主服务总线.md；调用链复用 Python 启动器的
//! `scripts/nixport_host.py`：rootfs 自带 `/usr/local/bin/nxp-port`（宿主总线的客机命令行
//! 客户端），经 rootfs 解释器运行它，stdout 返回客机可见路径（`/sdcard/...` 形式）。
//! 总线不可用（无会话/宿主太老/工具不在）时 [`available`] 为 false，调用方回退
//! rfd 对话框——与上游 PC 行为一致。

use std::path::{Path, PathBuf};
use std::process::Command;

const NXP_PORT: &str = "nxp-port";
/// nxp-port 的退出码（宿主服务总线.md）。
const EXIT_OK: i32 = 0;
const EXIT_CANCELLED: i32 = 9;
/// 3=无总线 4=连不上 5=总线已关：这一轮没有宿主总线，回退自带对话框。
const NO_BUS: [i32; 3] = [3, 4, 5];

fn fxd_root() -> Option<String> {
    let root = std::env::var("FXD_ROOT").ok()?;
    (!root.is_empty()).then_some(root)
}

/// 运行 nxp-port 的候选链：先经 rootfs 解释器（guest 里 ELF 解释器不可见），
/// 再裸 python，最后 PATH 上的 nxp-port（与 scripts/nixport_host.py 的候选链一致）。
fn candidates(action: &str, arguments: &[String]) -> Vec<Vec<String>> {
    let mut tail = vec![action.to_string()];
    tail.extend(arguments.iter().cloned());
    let mut out = Vec::new();
    if let Some(rootfs) = fxd_root() {
        let base = Path::new(&rootfs);
        let loader = base.join("usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1");
        let python = base.join("usr/bin/python3.13");
        let tool = base.join(format!("usr/local/bin/{NXP_PORT}"));
        if tool.is_file() && python.is_file() {
            let mut via_loader = vec![loader.to_string_lossy().into_owned()];
            via_loader.extend(tail.iter().cloned());
            if loader.is_file() {
                out.push(via_loader);
            }
            let mut direct = vec![python.to_string_lossy().into_owned()];
            direct.extend(tail.iter().cloned());
            out.push(direct);
        }
    }
    if let Some(found) = which(NXP_PORT) {
        let mut via_path = vec![found.to_string_lossy().into_owned()];
        via_path.extend(tail.iter().cloned());
        out.push(via_path);
    }
    out
}

fn which(name: &str) -> Option<PathBuf> {
    std::env::var_os("PATH").map(|paths| {
        std::env::split_paths(&paths)
            .map(|dir| dir.join(name))
            .find(|p| p.is_file())
    })?
}

/// 会话里有宿主总线 socket、且 rootfs（或 PATH）里有 nxp-port 时可用。
pub fn available() -> bool {
    if std::env::var("NXP_HOST_SOCKET").map_or(true, |s| s.is_empty()) {
        return false;
    }
    if let Some(rootfs) = fxd_root() {
        if Path::new(&rootfs)
            .join(format!("usr/local/bin/{NXP_PORT}"))
            .is_file()
        {
            return true;
        }
    }
    which(NXP_PORT).is_some()
}

/// 选择目录（宿主的应用内横屏选择屏）。`None`：取消 / 总线不可用。
/// 阻塞调用——放进后台任务（`blocking`）运行，别在 UI 线程上等。
pub fn pick_dir(initial: &Path) -> Option<PathBuf> {
    let arguments = [format!("--initial={}", initial.to_string_lossy())];
    for command in candidates("pick-dir", &arguments) {
        let Ok(done) = Command::new(&command[0]).args(&command[1..]).output() else {
            continue; // 这条链在这个宿主上跑不起来（解释器/工具不在），试下一条
        };
        let code = done.status.code().unwrap_or(-1);
        if NO_BUS.contains(&code) {
            return None; // 总线不在：这个会话没有宿主，不是调用方式的问题
        }
        if code != EXIT_OK && code != EXIT_CANCELLED {
            return None;
        }
        let text = String::from_utf8_lossy(&done.stdout).trim().to_string();
        return (!text.is_empty()).then_some(PathBuf::from(text));
    }
    None
}
