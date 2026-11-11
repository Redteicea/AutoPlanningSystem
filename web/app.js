// ---------------------------------------------------------------------------
// APS 前端 —— 原生 JS, 无框架无 CDN, 直接 fetch 后端 /api/*
// 结构: state 保存后端快照 -> 每个 render* 只读 state 渲染一块 UI
// ---------------------------------------------------------------------------
//
// 内容概要:
//   - state: 后端快照 (summary/problem/sol/events/zoom/busy)
//   - 工具: fmtTime/fmtNum/fmtPct/dayKey/colorOf/toast/api/withBusy/escapeHtml
//   - 数据加载: loadState/loadSchedule/loadEvents
//   - 渲染: renderSummary/renderKpis/renderGantt/renderOrders/renderUtil/renderEvents/renderLegend
//   - 交互: openModal/productOptions/machineOptions/bindActions
//   - 启动: main (IIFE) 加载并定时刷新
'use strict';

const state = {
  summary: null,   // /api/state -> summary
  problem: null,   // /api/state -> problem (订单/设备/工艺路线)
  sol: null,       // /api/schedule (排产结果)
  events: [],
  zoom: 26,        // 像素/小时
  busy: false,
};

const $ = (id) => document.getElementById(id);

// 工序颜色按产品取, 同一产品的工序同色, 甘特图上能一眼看出"哪批货在跑"
const PALETTE = ['#3b6ef0', '#0f9d76', '#c06a1c', '#8b5cf6', '#0e8ba8',
                 '#c2410c', '#4d7c0f', '#be3a6b', '#475569'];
/** 按 key 做确定性哈希取调色板颜色, 同一 key 始终同色 (甘特图按产品着色)。 */
function colorOf(key) {
  let h = 0;
  for (let i = 0; i < key.length; i++) h = (h * 31 + key.charCodeAt(i)) % 99991;
  return PALETTE[h % PALETTE.length];
}

// ---------------------------- 通用工具 ----------------------------
/** 把 Unix 秒格式化为 "MM-DD HH:MM"; 非法值返回 "—"; withYear 时加年份。 */
function fmtTime(sec, withYear) {
  if (sec === null || sec === undefined || sec < 0) return '—';
  const d = new Date(sec * 1000);
  const p = (n) => String(n).padStart(2, '0');
  const base = `${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`;
  return withYear ? `${d.getFullYear()}-${base}` : base;
}
/** 格式化数字 (固定小数位); 空/NaN 返回 "—"。 */
function fmtNum(v, digits = 1) {
  if (v === null || v === undefined || Number.isNaN(v)) return '—';
  return Number(v).toFixed(digits);
}
/** 把比率 v 格式化为百分比字符串 (如 0.982 -> "98.2%")。 */
function fmtPct(v, digits = 1) { return (v * 100).toFixed(digits) + '%'; }
/** 把 Unix 秒归到自然日, 返回 "YYYY-M-D" 形式的日期键。 */
function dayKey(sec) {
  const d = new Date(sec * 1000);
  return `${d.getFullYear()}-${d.getMonth() + 1}-${d.getDate()}`;
}

let toastTimer = null;
/** 弹出底部提示条; isErr 时红样式且停留更久; 自动定时消失。 */
function toast(msg, isErr) {
  const el = $('toast');
  el.textContent = msg;
  el.className = 'toast show' + (isErr ? ' err' : '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.className = 'toast'; }, isErr ? 5200 : 2600);
}

/** 调用后端接口: 无 body 用 GET, 有 body 用 POST(JSON); 解析 JSON 并在
 *  HTTP 非 2xx 或 data.ok=false 时抛出带错误信息的异常。 */
async function api(path, body) {
  const opt = body === undefined ? {} : {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  };
  const res = await fetch(path, opt);
  let data = null;
  try { data = await res.json(); } catch (e) { data = null; }
  if (!res.ok || !data || data.ok === false) {
    throw new Error((data && (data.error || data.message)) || `HTTP ${res.status}`);
  }
  return data;
}

/** 防重入执行: 置 busy 并临时改按钮文字, 执行 fn, 异常转 toast, 最终复原。 */
function withBusy(btn, text, fn) {
  if (state.busy) return;
  state.busy = true;
  const old = btn ? btn.textContent : '';
  if (btn) { btn.disabled = true; btn.textContent = text; }
  Promise.resolve().then(fn).catch((e) => toast(e.message || String(e), true))
    .finally(() => {
      state.busy = false;
      if (btn) { btn.disabled = false; btn.textContent = old; }
    });
}

// ---------------------------- 数据加载 ----------------------------
/** 拉取 /api/state, 写入 state.summary 与 state.problem。 */
async function loadState() {
  const d = await api('/api/state');
  state.summary = d.summary;
  state.problem = d.problem;
}

/** 拉取 /api/schedule, 有结果则写入 state.sol, 否则置空。 */
async function loadSchedule() {
  const d = await api('/api/schedule');
  state.sol = d && d.solved ? d : null;
}

/** 拉取 /api/events, 反转后存入 state.events (最新在前)。 */
async function loadEvents() {
  try {
    const d = await api('/api/events');
    state.events = (d.events || []).slice().reverse();
  } catch (e) { state.events = []; }
}

// ---------------------------- 顶栏 / KPI ----------------------------
/** 渲染顶栏: 车间名与订单/工序/设备/最近算法统计。 */
function renderSummary() {
  const s = state.summary || {};
  $('problemName').textContent = s.problemName || '车间排产';
  $('summary').innerHTML = [
    `订单 <b>${s.orders ?? 0}</b>`,
    `工序 <b>${s.ops ?? 0}</b>`,
    `设备 <b>${s.machines ?? 0}</b>`,
    `最近算法 <b>${s.lastAlgo || '—'}</b>`,
  ].join('');
}

/** 生成单张 KPI 卡片的 HTML 片段 (标签/值/单位/状态色/副说明)。 */
function kpiCard(label, value, unit, cls, sub) {
  return `<div class="kpi ${cls || ''}">
    <div class="k">${label}</div>
    <div class="v">${value}${unit ? `<span class="u">${unit}</span>` : ''}</div>
    ${sub ? `<div class="k" style="margin-top:2px">${sub}</div>` : ''}
  </div>`;
}

/** 渲染 8 张 KPI 卡片; 无结果时显示占位, 否则按 OTD/延期给出状态配色。 */
function renderKpis() {
  const el = $('kpis');
  if (!state.sol) {
    el.innerHTML = Array.from({ length: 8 }, () =>
      kpiCard('—', '—', '', '', '')).join('');
    return;
  }
  const k = state.sol.kpi;
  const otdCls = k.otd >= 0.98 ? 'good' : (k.otd >= 0.9 ? 'warn' : 'bad');
  const tardCls = k.totalTardinessHours <= 0.01 ? 'good' : (k.totalTardinessHours < 24 ? 'warn' : 'bad');
  el.innerHTML = [
    kpiCard('准时交付率 OTD', fmtPct(k.otd), '', otdCls,
      `${k.ordersOnTime}/${k.ordersTotal} 单准时`),
    kpiCard('总延期', fmtNum(k.totalTardinessHours, 1), 'h', tardCls,
      `延期订单 ${k.ordersLate} 单`),
    kpiCard('最大延期', fmtNum(k.maxTardinessHours, 1), 'h',
      k.maxTardinessHours > 0 ? 'warn' : 'good', '单笔最晚交付'),
    kpiCard('完工跨度', fmtNum(k.makespanHours, 1), 'h', '', '首工序→末工序'),
    kpiCard('换型次数', k.setupCount, '次', '',
      `换型 ${fmtNum(k.setupMinutes, 0)} 分钟`),
    kpiCard('平均设备负荷', fmtPct(k.avgUtilization), '', '',
      `峰值在制 ${k.peakWip}`),
    kpiCard('目标函数值', fmtNum(k.objective, 1), '', '',
      '越小越好 (延期+换型+跨度)'),
    kpiCard('求解耗时', fmtNum(k.solveMs, 0), 'ms',
      k.solveMs > 0 ? '' : '',
      k.evaluations > 0 ? `解码 ${k.evaluations} 次` : (state.sol.feasible ? '可行解' : '注意: 不可行')),
  ].join('');
}

// ---------------------------- 甘特图 ----------------------------
/** 渲染甘特图: 计算时间轴范围 -> 按设备分行 -> 绘制工序条(含换型段)与
 *  日界线/当前时刻线 -> 绑定悬停明细; 无结果时显示空提示。 */
function renderGantt() {
  const host = $('gantt');
  const tip = $('ganttTip');
  if (!state.sol || !state.sol.tasks || !state.sol.tasks.length) {
    host.innerHTML = '<div class="empty">还没有排产结果，点右上角「开始排产」</div>';
    tip.textContent = '方块=一道工序；浅色部分为换型准备时间。';
    return;
  }
  const tasks = state.sol.tasks;
  const machines = (state.problem && state.problem.machines) || [];

  let tmin = Infinity, tmax = -Infinity;
  for (const t of tasks) {
    tmin = Math.min(tmin, t.start - t.setup);
    tmax = Math.max(tmax, t.end);
  }
  const now = state.summary ? state.summary.serverTime : null;
  if (now) { tmin = Math.min(tmin, now); tmax = Math.max(tmax, now); }
  tmin = Math.floor(tmin / 3600) * 3600 - 3600;
  tmax = Math.ceil(tmax / 3600) * 3600 + 1800;

  const px = state.zoom / 3600;                // 秒 -> 像素
  const width = Math.max(320, (tmax - tmin) * px);
  const byMachine = new Map();
  for (const m of machines) byMachine.set(m.code, []);
  for (const t of tasks) {
    if (!byMachine.has(t.machine)) byMachine.set(t.machine, []);
    byMachine.get(t.machine).push(t);
  }

  // 日界线 + 日期标签
  const days = [];
  let cur = new Date(tmin * 1000);
  cur.setHours(0, 0, 0, 0);
  if (cur.getTime() / 1000 < tmin) cur.setDate(cur.getDate() + 1);
  while (cur.getTime() / 1000 <= tmax) {
    days.push(Math.round(cur.getTime() / 1000));
    cur = new Date(cur.getTime() + 86400000);
  }

  const rows = [];
  // 表头: 日期刻度
  let head = '<div class="g-row head"><div class="g-label" style="border-right:0"></div><div class="g-track" style="width:' + width + 'px">';
  for (const d of days) {
    const x = (d - tmin) * px;
    head += `<div class="g-grid day" style="left:${x}px"></div>`;
    head += `<div class="g-daylabel" style="left:${x}px">${fmtTime(d).slice(0, 5)}</div>`;
  }
  if (now) head += `<div class="g-grid now" style="left:${(now - tmin) * px}px"></div>`;
  head += '</div></div>';
  rows.push(head);

  for (const m of machines) {
    const list = (byMachine.get(m.code) || []).slice().sort((a, b) => a.start - b.start);
    let track = `<div class="g-row"><div class="g-label ${m.down ? 'down' : ''}">
        <span class="code">${m.code}</span><span class="grp">${m.group || ''}</span>
      </div><div class="g-track" style="width:${width}px">`;
    for (const d of days) track += `<div class="g-grid" style="left:${(d - tmin) * px}px"></div>`;
    if (now) track += `<div class="g-grid now" style="left:${(now - tmin) * px}px"></div>`;

    for (const t of list) {
      const occStart = t.start - t.setup;
      const left = (occStart - tmin) * px;
      const wOcc = Math.max(2, (t.end - occStart) * px);
      const leftWork = (t.start - occStart) * px;
      const wWork = Math.max(1, (t.end - t.start) * px);
      const c = colorOf(t.product || t.order);
      const cls = ['bar'];
      if (t.frozen) cls.push('frozen');
      if (t.urgent) cls.push('urgent');
      track += `<div class="${cls.join(' ')}" data-op="${t.op}"
          style="left:${left}px;width:${wOcc}px;background:${c}33;border-color:${c}55">
          <div class="work" style="left:${leftWork}px;width:${wWork}px;background:${c}"></div>
          <span class="lbl" style="left:${leftWork + 3}px">${wWork > 46 ? escapeHtml(t.order) : ''}</span>
        </div>`;
    }
    track += '</div></div>';
    rows.push(track);
  }
  host.innerHTML = rows.join('');

  tip.textContent = `时间范围 ${fmtTime(tmin)} → ${fmtTime(tmax)}　|　共 ${tasks.length} 道工序　|　浅色段=换型准备，蓝竖线=当前时刻`;

  // 悬停明细
  const tipEl = $('tooltip');
  host.querySelectorAll('.bar').forEach((bar) => {
    const t = tasks[Number(bar.dataset.op)];
    if (!t) return;
    bar.addEventListener('mouseenter', (ev) => {
      tipEl.innerHTML =
        `<b>${escapeHtml(t.order)}</b> · ${escapeHtml(t.product)}<br>` +
        `${t.seq + 1}. ${escapeHtml(t.name)}　<i>${escapeHtml(t.family)}</i><br>` +
        `设备 ${escapeHtml(t.machine)}　数量 ${t.qty}<br>` +
        `${fmtTime(t.start)} → ${fmtTime(t.end)}<br>` +
        `加工 ${fmtNum(t.duration / 60, 0)} min` +
        (t.setup ? `　换型 ${fmtNum(t.setup / 60, 0)} min` : '') +
        (t.urgent ? '　<span style="color:#ffd54a">急单</span>' : '') +
        (t.frozen ? '　已冻结' : '');
      tipEl.classList.add('show');
      moveTip(ev);
    });
    bar.addEventListener('mousemove', moveTip);
    bar.addEventListener('mouseleave', () => tipEl.classList.remove('show'));
  });
}

/** 跟随鼠标定位悬浮提示框, 并避免超出视口右/下边界。 */
function moveTip(ev) {
  const el = $('tooltip');
  const pad = 14;
  let x = ev.clientX + pad, y = ev.clientY + pad;
  const r = el.getBoundingClientRect();
  if (x + r.width > window.innerWidth - 8) x = ev.clientX - r.width - pad;
  if (y + r.height > window.innerHeight - 8) y = ev.clientY - r.height - pad;
  el.style.left = x + 'px';
  el.style.top = y + 'px';
}

/** 转义 HTML 特殊字符, 防止订单号/详情中的 <>& 破坏 DOM 或注入。 */
function escapeHtml(s) {
  return String(s === undefined || s === null ? '' : s)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

// ---------------------------- 订单表 / 负荷 / 事件 ----------------------------
/** 渲染订单明细表: 按准时优先、延期时长排序, 标注延期与急单。 */
function renderOrders() {
  const tbody = document.querySelector('#orderTable tbody');
  if (!state.sol || !state.sol.orders) {
    tbody.innerHTML = '<tr><td colspan="8" class="muted">暂无结果</td></tr>';
    $('orderHint').textContent = '';
    return;
  }
  const orders = state.sol.orders.slice().sort((a, b) => {
    if (a.onTime !== b.onTime) return a.onTime ? 1 : -1;
    return b.tardinessHours - a.tardinessHours;
  });
  tbody.innerHTML = orders.map((o) => `
    <tr>
      <td>${escapeHtml(o.code)}</td>
      <td>${escapeHtml(o.product)}</td>
      <td class="n">${o.qty}</td>
      <td class="n">${o.priority}${o.urgent ? ' <span class="pill urg">急</span>' : ''}</td>
      <td>${fmtTime(o.due)}</td>
      <td>${fmtTime(o.completion)}</td>
      <td class="n" style="color:${o.tardinessHours > 0 ? '#b62c2c' : '#8b94a3'}">${fmtNum(o.tardinessHours, 1)}</td>
      <td>${o.onTime ? '<span class="pill ok">准时</span>' : '<span class="pill late">延期</span>'}</td>
    </tr>`).join('');
  const late = orders.filter((o) => !o.onTime).length;
  $('orderHint').textContent = `${orders.length} 单，延期 ${late} 单`;
}

/** 渲染设备负荷条: 按利用率降序, 停机设备特殊标记。 */
function renderUtil() {
  const el = $('util');
  if (!state.sol || !state.sol.machineUtilization) { el.innerHTML = '<div class="muted">暂无结果</div>'; return; }
  const downSet = new Set(((state.problem && state.problem.machines) || []).filter((m) => m.down).map((m) => m.code));
  const rows = state.sol.machineUtilization.slice().sort((a, b) => b.utilization - a.utilization);
  el.innerHTML = rows.map((m) => {
    const down = downSet.has(m.code);
    const pct = Math.max(0, Math.min(1, m.utilization)) * 100;
    return `<div class="u-row ${down ? 'down' : ''}">
      <span>${escapeHtml(m.code)}${down ? ' <span class="pill late">停</span>' : ''}</span>
      <span class="bar-bg"><span class="bar-fg" style="width:${pct.toFixed(1)}%"></span></span>
      <span class="pct">${pct.toFixed(0)}%</span>
    </div>`;
  }).join('');
}

/** 渲染事件流列表; 按类型给出颜色 (breakdown 红 / insert 黄)。 */
function renderEvents() {
  const el = $('events');
  if (!state.events.length) { el.innerHTML = '<li class="muted">暂无事件</li>'; return; }
  el.innerHTML = state.events.map((e) => {
    const cls = e.type === 'breakdown' ? 'bad' : (e.type === 'insert' ? 'warn' : '');
    return `<li><span class="t">${(e.atText || '').slice(5, 16)}</span>
      <span class="k ${cls}">${escapeHtml(e.type)}</span>
      <span>${escapeHtml(e.detail)}</span></li>`;
  }).join('');
}

/** 渲染甘特图图例: 列出出现过的产品与其颜色 (最多 6 项)。 */
function renderLegend() {
  const el = $('legend');
  if (!state.sol || !state.sol.tasks) { el.innerHTML = ''; return; }
  const seen = new Map();
  for (const t of state.sol.tasks) if (!seen.has(t.product)) seen.set(t.product, colorOf(t.product));
  el.innerHTML = Array.from(seen.entries()).slice(0, 6)
    .map(([p, c]) => `<span><i style="background:${c}"></i>${escapeHtml(p)}</span>`).join('');
}

/** 依次重渲染全部视图 (摘要/KPI/甘特/订单/负荷/事件/图例)。 */
function renderAll() {
  renderSummary();
  renderKpis();
  renderGantt();
  renderOrders();
  renderUtil();
  renderEvents();
  renderLegend();
}

// ---------------------------- 交互 ----------------------------
/** 打开通用模态框: 按 fields 动态生成表单 (input/select), 确认时收集值并
 *  回调 onOk; 取消或确认后关闭。 */
function openModal(title, fields, onOk) {
  $('modalTitle').textContent = title;
  const body = $('modalBody');
  body.innerHTML = fields.map((f) => {
    if (f.type === 'select') {
      const opts = f.options.map((o) =>
        `<option value="${escapeHtml(o.value)}"${o.value === f.value ? ' selected' : ''}>${escapeHtml(o.label)}</option>`).join('');
      return `<label>${f.label}<select id="f_${f.key}">${opts}</select></label>`;
    }
    return `<label>${f.label}<input id="f_${f.key}" type="${f.type || 'text'}" value="${escapeHtml(f.value ?? '')}"></label>`;
  }).join('');

  const modal = $('modal');
  modal.classList.add('show');
  const close = () => { modal.classList.remove('show'); $('modalOk').onclick = null; };
  $('modalCancel').onclick = close;
  $('modalOk').onclick = () => {
    const data = {};
    for (const f of fields) {
      const input = $('f_' + f.key);
      data[f.key] = f.type === 'number' ? Number(input.value) : input.value;
    }
    close();
    onOk(data);
  };
}

/** 从 state.problem 提取产品下拉项 (value=product, label=product)。 */
function productOptions() {
  const list = (state.problem && state.problem.routings) || [];
  return list.map((r) => ({ value: r.product, label: r.product }));
}

/** 从 state.problem 提取可用 (未停机) 设备下拉项, 标注所属组。 */
function machineOptions() {
  const list = (state.problem && state.problem.machines) || [];
  return list.filter((m) => !m.down).map((m) => ({ value: m.code, label: `${m.code} (${m.group})` }));
}

/** 绑定全部按钮交互: 排产/重置/插单/故障/基准测试/缩放。 */
function bindActions() {
  $('btnSolve').onclick = (ev) => withBusy(ev.currentTarget, '排产中…', async () => {
    const body = {
      algo: $('algo').value,
      timeLimitMs: Number($('budget').value),
    };
    const t0 = performance.now();
    const d = await api('/api/schedule', body);
    await loadState();
    state.sol = d.solved ? d : null;
    await loadEvents();
    renderAll();
    toast(`排产完成：${d.solver || d.algo || ''} 用时 ${Math.round(performance.now() - t0)} ms，校验 ${d.validation || '—'}`);
  });

  $('btnReset').onclick = () => {
    openModal('重置演示算例', [
      { key: 'orders', label: '订单数量', type: 'number', value: 80 },
      { key: 'days', label: '排产天数', type: 'number', value: 14 },
      { key: 'seed', label: '随机种子', type: 'number', value: 20261105 },
    ], (d) => withBusy(null, '', async () => {
      await api('/api/demo/reset', { orders: d.orders, days: d.days, seed: d.seed });
      await loadState();
      await loadSchedule();
      await loadEvents();
      renderAll();
      toast('演示算例已重建，记得重新排产');
    }));
  };

  /** "8-12,13-17" 形式的班次串 -> [{start,end}]；非法格式抛错。 */
  function parseShifts(text) {
    const shifts = String(text || '').split(',').map((s) => s.trim()).filter(Boolean)
      .map((s) => {
        const m = s.match(/^(\d{1,2})\s*[-~]\s*(\d{1,2})$/);
        if (!m) throw new Error(`班次格式不对："${s}"（应为 8-12 这种）`);
        return { start: Number(m[1]), end: Number(m[2]) };
      });
    if (!shifts.length) throw new Error('至少要有一个班次');
    return shifts;
  }

  $('btnConfig').onclick = async () => {
    let cfg;
    try {
      cfg = await api('/api/config');          // GET: 读当前生效配置
    } catch (e) {
      toast('读取配置失败：' + e.message, true);
      return;
    }
    const cal = cfg.calendar || {};
    const demo = cfg.demo || {};
    const shiftsText = (cal.shifts || []).map((s) => `${s.start}-${s.end}`).join(',');
    openModal('通用配置（写入 config.json，立即生效）', [
      { key: 'workdays', label: '工作日 (0=周日, 逗号分隔)', value: (cal.workdays || []).join(',') },
      { key: 'shifts', label: '班次 (格式 8-12,13-17；22-6 表跨夜班)', value: shiftsText },
      { key: 'orders', label: '订单数量', type: 'number', value: demo.orders ?? 80 },
      { key: 'days', label: '排产天数', type: 'number', value: demo.days ?? 14 },
    ], (d) => withBusy(null, '', async () => {
      const workdays = String(d.workdays).split(',').map((s) => Number(s.trim())).filter((n) => Number.isInteger(n) && n >= 0 && n <= 6);
      const body = {
        calendar: { workdays, shifts: parseShifts(d.shifts) },
        demo: { orders: d.orders, days: d.days },
      };
      const r = await api('/api/config', body);   // POST: 校验+落盘+重建算例
      await loadState();
      await loadSchedule();
      await loadEvents();
      renderAll();
      toast(`配置已写入 config.json 并重建算例（${r.summary?.orders ?? '?'} 订单），记得重新排产`);
    }));
  };

  $('btnInsert').onclick = () => {
    const products = productOptions();
    if (!products.length) { toast('没有可用工艺路线', true); return; }
    openModal('插入急单（会冻结已开工工序，只动未开工的）', [
      { key: 'code', label: '订单号', value: '' },
      { key: 'product', label: '产品', type: 'select', options: products, value: products[0].value },
      { key: 'qty', label: '数量', type: 'number', value: 1200 },
      { key: 'dueDays', label: '交期 (几天后)', type: 'number', value: 2 },
    ], (d) => withBusy(null, '', async () => {
      const now = Math.floor(Date.now() / 1000);
      const r = await api('/api/reschedule/insert', {
        code: d.code,
        product: d.product,
        qty: d.qty,
        due: now + d.dueDays * 86400,
      });
      state.sol = r.solved ? r : state.sol;
      await loadState();
      await loadEvents();
      renderAll();
      const rs = r.reschedule || {};
      toast(`插单完成：${r.message || ''}${rs.movedOps !== undefined ? `；漂移 ${rs.movedOps} 道` : ''}`);
    }));
  };

  $('btnDown').onclick = () => {
    const machines = machineOptions();
    if (!machines.length) { toast('没有可停机的设备', true); return; }
    openModal('设备故障（立即停机并重排）', [
      { key: 'machine', label: '故障设备', type: 'select', options: machines, value: machines[0].value },
    ], (d) => withBusy(null, '', async () => {
      const r = await api('/api/reschedule/breakdown', { machine: d.machine });
      state.sol = r.solved ? r : state.sol;
      await loadState();
      await loadEvents();
      renderAll();
      toast(`已停机并重排：${r.message || ''}`);
    }));
  };

  $('btnBench').onclick = (ev) => withBusy(ev.currentTarget, '基准测试中…', async () => {
    const d = await api('/api/benchmark', { timeLimitMs: Number($('budget').value) });
    const tbody = document.querySelector('#benchTable tbody');
    tbody.innerHTML = (d.rows || []).map((r) => `
      <tr>
        <td class="n">${r.orders}</td>
        <td class="n">${r.ops}</td>
        <td class="n">${r.machines}</td>
        <td class="n">${fmtNum(r.ruleMs, 1)}</td>
        <td class="n">${fmtNum(r.gaMs, 0)}</td>
        <td class="n">${fmtNum(r.lnsMs, 0)}</td>
        <td class="n">${fmtNum(r.ruleObjective, 1)}</td>
        <td class="n">${fmtNum(r.gaObjective, 1)}</td>
        <td class="n">${fmtNum(r.lnsObjective, 1)}</td>
        <td class="n" style="color:${r.improvement >= 0 ? '#0e7d3f' : '#b62c2c'}">${(r.improvement * 100).toFixed(1)}%</td>
        <td class="n" style="color:${r.lnsImprovement >= 0 ? '#0e7d3f' : '#b62c2c'}">${(r.lnsImprovement * 100).toFixed(1)}%</td>
      </tr>`).join('') || '<tr><td colspan="11" class="muted">无数据</td></tr>';
    toast(`基准完成：${(d.rows || []).length} 档规模，${d.cpuThreads} 线程`);
  });

  document.querySelectorAll('#zoom button').forEach((b) => {
    b.onclick = () => {
      document.querySelectorAll('#zoom button').forEach((x) => x.classList.remove('on'));
      b.classList.add('on');
      state.zoom = Number(b.dataset.zoom);
      renderGantt();
    };
  });
}

// ---------------------------- 启动 ----------------------------
/** 启动入口 (IIFE): 绑定交互 -> 加载并渲染 -> 每 15s 静默刷新状态与事件。 */
(async function main() {
  bindActions();
  try {
    await loadState();
    await Promise.all([loadSchedule(), loadEvents()]);
    renderAll();
  } catch (e) {
    toast('连不上后端服务: ' + e.message, true);
  }
  setInterval(async () => {
    try {
      await Promise.all([loadState(), loadEvents()]);
      renderEvents();
      renderSummary();
    } catch (e) { /* 静默 */ }
  }, 15000);
})();
