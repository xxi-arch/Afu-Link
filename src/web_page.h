#pragma once

const char kWebPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>阿福体重记录</title>
<style>
:root{color-scheme:light;font-family:system-ui,sans-serif;color:#16342d;background:#f2f6f3}
*{box-sizing:border-box}body{margin:0}
main{max-width:880px;margin:auto;padding:clamp(20px,5vw,48px)}
header{display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap;margin-bottom:22px}
h1,h2,p{margin:0}h1{font-size:clamp(24px,4vw,34px);letter-spacing:-.04em}h2{font-size:18px}
.status{background:#e1efe6;border-radius:20px;padding:7px 12px;font-size:13px;color:#215940}
.overview{display:grid;grid-template-columns:2fr 1fr;gap:14px;margin-bottom:16px}
.card{background:#fff;border:1px solid #dce8e0;border-radius:15px;padding:20px;margin-bottom:16px}
.overview .card{margin:0}.latest{background:#183c31;color:#f4fff7;border-color:#183c31}
.label{font-size:14px;color:#52695e}.latest .label,.latest small{color:#c9e1d0}
.value{font-size:clamp(32px,6vw,54px);font-weight:650;font-variant-numeric:tabular-nums;letter-spacing:-.05em;margin:3px 0}
.value small{font-size:16px;font-weight:500;letter-spacing:0}
.bar{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:13px}
.buttons{display:flex;gap:8px;flex-wrap:wrap}
button{border:1px solid #b8d1c2;border-radius:8px;background:#edf5ef;color:#174b39;padding:7px 12px;font:inherit;cursor:pointer}
button:disabled{opacity:.5;cursor:default}
button:focus-visible,select:focus-visible,input:focus-visible,summary:focus-visible{outline:2px solid #337957;outline-offset:2px}
table{border-collapse:collapse;text-align:left;width:100%;font-size:14px;font-variant-numeric:tabular-nums}
th,td{padding:10px 10px;border-bottom:1px solid #e6eee8;white-space:nowrap}
th{color:#52695e;font-weight:550}td:nth-child(2){font-weight:650}.table{overflow-x:auto}
.message{padding:14px 0;color:#52695e;font-size:14px}.error{color:#a52625}
summary{cursor:pointer;font-weight:600}
form{display:flex;flex-wrap:wrap;align-items:end;gap:12px;margin-top:12px}label{display:grid;gap:5px;font-size:14px}
.check-label{display:flex;align-items:center;gap:8px}.check-label input{max-width:none}
.ha-form{display:grid;grid-template-columns:1fr;align-items:stretch;max-width:560px}
.ha-form label{max-width:100%}
.ha-form input[type=url],.ha-form input[type=password]{width:100%;max-width:100%}
.ha-form button{justify-self:start}
input,select{padding:7px;border:1px solid #a8c4b4;border-radius:8px;background:white;font:inherit;max-width:135px}
dialog{width:min(540px,calc(100% - 32px));max-height:calc(100% - 32px);overflow:auto;border:1px solid #dce8e0;border-radius:15px;padding:20px;color:inherit}
dialog::backdrop{background:#0c2e2599}
dialog form{margin:0}.metric-source{font-size:14px;color:#52695e}
.metrics{display:grid;grid-template-columns:repeat(auto-fit,minmax(155px,1fr));gap:9px;margin-bottom:0}
.metrics div{background:#f2f6f3;border-radius:9px;padding:10px}.metrics dt{font-size:13px;color:#52695e}
.metrics dd{margin:3px 0 0;font-weight:600;font-variant-numeric:tabular-nums}
@media(max-width:480px){main{padding:16px}.card{padding:14px}.overview{gap:10px}.bar{align-items:start;flex-wrap:wrap}}
</style>
</head>
<body>
<main>
<header><h1>阿福体重记录</h1><span id="status" class="status" role="status">正在连接</span></header>
<section class="overview" aria-label="测量概览">
  <div class="card latest"><p class="label">最近一次体重</p><p class="value"><span id="weight">—</span> <small>kg</small></p><small id="latestTime">—</small></div>
  <div class="card"><p class="label">已保存测量</p><p class="value" id="count">—</p></div>
</section>
<section class="card">
  <div class="bar"><h2>测量历史</h2><div class="buttons"><button id="newest">最新</button><button id="older" disabled>更早</button></div></div>
  <p class="message" id="message" role="status"></p>
  <div class="table"><table>
    <thead><tr><th scope="col">测量时间</th><th scope="col">体重 kg</th><th scope="col">BMI</th><th scope="col">体脂率</th><th scope="col">阻抗原始值 (ADC)</th><th scope="col">体成分</th></tr></thead>
    <tbody id="rows"></tbody>
  </table></div>
</section>
<details class="card">
  <summary>体脂秤资料</summary>
  <p class="message">填写身高、年龄和性别后，C3 会为后续称重估算体脂等指标。资料未填齐时只保存称重数据。</p>
  <form id="profile">
    <label>身高 (cm)<input name="height" type="number" required min="80" max="230"></label>
    <label>年龄<input name="age" type="number" required min="5" max="120"></label>
    <label>性别<select name="sex" required><option value="">请选择</option><option value="1">男</option><option value="2">女</option></select></label>
    <button type="submit">保存资料</button>
  </form>
  <p class="message" id="profileMessage" role="status"></p>
</details>
<details class="card">
  <summary>Home Assistant 接入</summary>
  <p class="message">填写 HA 根地址和长期访问令牌即可上传，不需要安装 MQTT 或其他插件。令牌在 HA 个人资料 → 安全 → 长期访问令牌中创建，请使用管理员账号。</p>
  <form id="haConfig" class="ha-form">
    <label>Home Assistant 地址
      <input name="url" type="url" maxlength="127" placeholder="http://192.168.1.10:8123" autocomplete="url">
    </label>
    <label>长期访问令牌（Token）
      <input name="token" type="password" maxlength="255" autocomplete="new-password" placeholder="粘贴 HA 创建的长期访问令牌">
    </label>
    <label class="check-label"><input name="enabled" type="checkbox">启用自动上传</label>
    <label class="check-label"><input name="clearToken" type="checkbox">清除已保存的 Token</label>
    <button type="submit">保存并测试连接</button>
  </form>
  <p class="message" id="haMessage" role="status">正在读取配置…</p>
  <p class="message">目前使用局域网 HTTP 地址；HTTPS 和公网反向代理暂不支持。Token 会保存在 C3 本机，不会通过配置读取接口返回。</p>
</details>
</main>
<dialog id="bodyDialog" aria-labelledby="bodyTitle">
  <div class="bar"><h2 id="bodyTitle">体成分详情</h2><form method="dialog"><button>关闭</button></form></div>
  <p id="bodyContext" class="metric-source"></p>
  <dl id="metricList" class="metrics"></dl>
</dialog>
<script>
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

async function refresh() {
  const current = ++request;
  try {
    const response = await fetch("/api/measurements?before=" + before, {cache:"no-store"});
    if (!response.ok) throw Error();
    const data = await response.json();
    if (current !== request) return;
    byId("status").textContent = states[data.ble] || "蓝牙状态未知";
    byId("count").textContent = data.count;
    if (!before && data.rows.length) {
      byId("weight").textContent = weightLabel(data.rows[0].centiKg);
      byId("latestTime").textContent = timeLabel(data.rows[0].time);
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
async function loadHaConfig() {
  try {
    const response = await fetch("/api/ha", {cache:"no-store"});
    if (!response.ok) throw Error();
    const data = await response.json();
    haForm.elements.enabled.checked = data.enabled;
    haForm.elements.url.value = data.url ?? "";
    byId("haMessage").textContent =
      (data.tokenSet ? "Token 已保存。" : "尚未设置 Token。") + " 当前状态：" + data.status;
  } catch {
    byId("haMessage").textContent = "无法读取 Home Assistant 配置。";
  }
}
haForm.onsubmit = async (event) => {
  event.preventDefault();
  const button = haForm.querySelector("button[type=submit]");
  button.disabled = true;
  const params = new URLSearchParams();
  params.set("enabled", haForm.elements.enabled.checked ? "1" : "0");
  params.set("url", haForm.elements.url.value.trim());
  params.set("token", haForm.elements.token.value.trim());
  if (haForm.elements.clearToken.checked) params.set("clearToken", "1");
  byId("haMessage").textContent = "正在保存并测试连接…";
  try {
    const response = await fetch("/api/ha", {method:"POST",body:params});
    const data = await response.json();
    if (!response.ok) throw Error(data.error || "保存失败");
    haForm.elements.token.value = "";
    haForm.elements.clearToken.checked = false;
    byId("haMessage").textContent =
      (data.tokenSet ? "Token 已保存。" : "尚未设置 Token。") + " 当前状态：" + data.status;
  } catch (error) {
    byId("haMessage").textContent = "保存失败：" + error.message;
  } finally {
    button.disabled = false;
  }
};
loadHaConfig();
refresh();
setInterval(() => {
  if (!before && !document.hidden && !bodyDialog.open) refresh();
  if (!document.hidden && haForm.closest("details").open) loadHaConfig();
}, 3500);
</script>
</body>
</html>)HTML";
