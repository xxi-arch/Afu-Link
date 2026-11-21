const byId = (id) => document.getElementById(id);
const states = {starting:"蓝牙启动中",scanning:"等待体重秤",connecting:"正在连接体重秤",receiving:"已连接体重秤",error:"蓝牙启动失败"};
const localTime = new Intl.DateTimeFormat("zh-CN",{timeZone:"Asia/Shanghai",year:"numeric",month:"2-digit",day:"2-digit",hour:"2-digit",minute:"2-digit",second:"2-digit",hour12:false});
const timeLabel = (time) => time ? localTime.format(new Date(time * 1000)) : "时间未同步";
const weightLabel = (value) => (value / 100).toFixed(2);
const bodyDialog = byId("bodyDialog");
const metricLabels = [
  ["fat","体脂"],["muscle","肌肉"],["skeletalMuscle","骨骼肌"],["water","水分"],
  ["protein","蛋白质"],["bone","骨量"],["subcutaneousFat","皮下脂肪"]
];
let before = 0, next = 0, request = 0, renderedRows = "";

function showBody(row) {
  const body = row.body;
  const method = body.method === "impedance" ? "基于阻抗估算" : "基于 BMI 估算";
  byId("bodyContext").textContent = timeLabel(row.time) + " · " + method;
  const entries = [["BMI",body.bmi.toFixed(2)]];
  for (const [key, label] of metricLabels) {
    const [percent, mass] = body[key];
    entries.push([label, percent.toFixed(1) + "% / " + mass.toFixed(1) + " kg"]);
  }
  const list = document.createDocumentFragment();
  for (const [label, text] of entries) {
    const item = document.createElement("div");
    const term = document.createElement("dt");
    const value = document.createElement("dd");
    term.textContent = label;
    value.textContent = text;
    item.append(term, value);
    list.append(item);
  }
  byId("metricList").replaceChildren(list);
  bodyDialog.showModal();
}

async function deleteMeasurement(row, button) {
  if (!window.confirm("删除这条称重记录？")) return;
  const message = byId("deleteMessage");
  message.textContent = "";
  button.disabled = true;
  try {
    const response = await fetch("/api/measurements?id=" + row.id, {method:"DELETE"});
    if (!response.ok && response.status !== 404) throw Error();
    await refresh();
  } catch {
    message.textContent = "删除失败，请重试。";
  } finally {
    button.disabled = false;
  }
}

async function refresh() {
  const current = ++request;
  try {
    const response = await fetch("/api/measurements?before=" + before, {cache:"no-store"});
    if (!response.ok) throw Error();
    const data = await response.json();
    if (current !== request) return;
    byId("status").textContent = states[data.ble] || "蓝牙状态未知";
    byId("count").textContent = data.count;
    if (!before || !data.count) {
      const latest = data.rows[0];
      byId("weight").textContent = latest ? weightLabel(latest.centiKg) : "—";
      byId("latestTime").textContent = latest ? timeLabel(latest.time) : "—";
    }
    next = data.next;
    byId("older").disabled = !next;
    byId("message").textContent = !data.writable ? "存储无法写入，请检查设备。" :
      !data.count ? "暂无记录。站上体重秤并等待读数稳定。" : "";
    byId("message").classList.toggle("error", !data.writable);
    const rowData = JSON.stringify(data.rows);
    if (renderedRows === rowData) return;
    renderedRows = rowData;
    const rows = document.createDocumentFragment();
    for (const row of data.rows) {
      const tr = document.createElement("tr");
      const bmi = row.body ? row.body.bmi.toFixed(2) :
        row.legacy && row.heightCm ? (row.centiKg / 100 / (row.heightCm / 100) ** 2).toFixed(1) : "—";
      const fat = row.body ? row.body.fat[0].toFixed(1) + "%" : "—";
      for (const value of [timeLabel(row.time), weightLabel(row.centiKg), bmi, fat, row.adc.join(" / ") || "—"]) {
        tr.insertCell().textContent = value;
      }
      if (row.source === "scale") {
        const source = document.createElement("small");
        source.className = "history-source";
        source.textContent = "秤内补录";
        tr.cells[0].append(source);
      }
      const details = tr.insertCell();
      if (row.body) {
        const button = document.createElement("button");
        button.textContent = "查看";
        button.setAttribute("aria-label", "查看 " + timeLabel(row.time) + " 的体成分");
        button.onclick = () => showBody(row);
        details.append(button);
      } else {
        details.textContent = "—";
      }
      const remove = document.createElement("button");
      remove.className = "delete-measurement";
      remove.textContent = "删除";
      remove.setAttribute("aria-label", "删除 " + timeLabel(row.time) + " 的称重记录");
      remove.onclick = () => deleteMeasurement(row, remove);
      tr.insertCell().append(remove);
      rows.append(tr);
    }
    byId("rows").replaceChildren(rows);
  } catch {
    if (current === request) byId("status").textContent = "设备连接中断";
  }
}
byId("newest").onclick = () => { before = 0; refresh(); };
byId("older").onclick = () => { before = next; refresh(); };
const form = byId("profile");
fetch("/api/profile", {cache:"no-store"}).then((r) => r.json()).then((data) => {
  for (const name of ["height","age","sex"]) {
    if (!form.elements[name].value && data[name]) form.elements[name].value = data[name];
  }
}).catch(() => {});
form.onsubmit = async (event) => {
  event.preventDefault();
  const button = form.querySelector("button");
  button.disabled = true;
  try {
    const response = await fetch("/api/profile", {method:"POST",body:new URLSearchParams(new FormData(form))});
    if (!response.ok) throw Error();
    byId("profileMessage").textContent = "资料已保存。";
  } catch {
    byId("profileMessage").textContent = "保存失败，请重试。";
  } finally {
    button.disabled = false;
  }
};

const haForm = byId("haConfig");
const haButton = haForm.querySelector("button[type=submit]");
const haStatus = byId("haStatus");
const haMessage = byId("haMessage");
const haSaveErrors = {
  invalid_url: "请输入局域网 Home Assistant 的 HTTP 根地址，例如 http://192.168.1.10:8123。",
  missing_token: "启用自动上传需要 Token。要清除 Token，请同时取消启用自动上传。",
  invalid_token: "Token 格式有误，请重新粘贴长期访问令牌。",
  token_too_long: "Token 太长，请核对粘贴内容。",
  storage_unavailable: "设备无法写入配置，请重试。"
};
let haConfigLoaded = false;
let haRead = null;

function showHaMessage(message, isError = false) {
  haMessage.textContent = message;
  haMessage.classList.toggle("error", isError);
}

function showHaStatus(data) {
  const message = (data.tokenSet ? "Token 已保存。" : "尚未设置 Token。") + " 当前状态：" + data.status;
  if (haStatus.textContent !== message) haStatus.textContent = message;
}

async function loadHaConfig() {
  if (haRead || haButton.disabled) return;
  const request = new AbortController();
  haRead = request;
  try {
    const response = await fetch("/api/ha", {cache:"no-store",signal:request.signal});
    if (!response.ok) throw Error();
    const data = await response.json();
    if (request.signal.aborted) return;
    if (!haConfigLoaded) {
      haForm.elements.enabled.defaultChecked = data.enabled;
      haForm.elements.url.defaultValue = data.url ?? "";
      haConfigLoaded = true;
    }
    showHaStatus(data);
  } catch {
    if (!request.signal.aborted) haStatus.textContent = "无法读取 Home Assistant 配置。";
  } finally {
    if (haRead === request) haRead = null;
  }
}
haForm.addEventListener("input", () => showHaMessage(""));
haForm.onsubmit = async (event) => {
  event.preventDefault();
  if (haButton.disabled) return;
  haButton.disabled = true;
  haRead?.abort();
  haRead = null;
  const tokenField = haForm.elements.token;
  const clearToken = haForm.elements.clearToken.checked;
  const token = tokenField.value;
  const params = new URLSearchParams();
  params.set("enabled", haForm.elements.enabled.checked ? "1" : "0");
  params.set("url", haForm.elements.url.value.trim());
  params.set("token", token.trim());
  if (clearToken) params.set("clearToken", "1");
  showHaMessage("正在保存…");
  try {
    const response = await fetch("/api/ha", {method:"POST",body:params});
    if (!response.ok) {
      const data = await response.json().catch(() => ({}));
      showHaMessage(haSaveErrors[data.error] ?? "保存失败，请重试。", true);
      return;
    }
    const data = await response.json();
    if (tokenField.value === token) tokenField.value = "";
    if (clearToken) haForm.elements.clearToken.checked = false;
    haConfigLoaded = true;
    showHaStatus(data);
    showHaMessage("配置已保存。");
  } catch {
    showHaMessage("无法确认配置是否已保存，请检查设备连接。", true);
  } finally {
    haButton.disabled = false;
  }
};
loadHaConfig();
refresh();
setInterval(() => {
  if (!before && !document.hidden && !bodyDialog.open) refresh();
  if (!document.hidden && haForm.closest("details").open) loadHaConfig();
}, 3500);
