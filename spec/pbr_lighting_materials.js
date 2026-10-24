(() => {
  "use strict";

  const PI = Math.PI;
  const DEG = PI / 180;
  const root = document.documentElement;

  // ---------------------------------------------------------------- math

  const clamp = (x, a, b) => Math.min(b, Math.max(a, x));
  const saturate = (x) => clamp(x, 0, 1);
  const lerp = (a, b, t) => a + (b - a) * t;
  const add = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
  const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
  const scale = (a, s) => [a[0] * s, a[1] * s, a[2] * s];
  const mulv = (a, b) => [a[0] * b[0], a[1] * b[1], a[2] * b[2]];
  const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
  const normalize = (v) => scale(v, 1 / (Math.hypot(v[0], v[1], v[2]) || 1));
  const reflect = (d, n) => sub(d, scale(n, 2 * dot(d, n)));
  const luminance = (c) => 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];

  // seeded rng so demos are reproducible between reloads
  function mulberry32(seed) {
    let a = seed >>> 0;
    return () => {
      a = (a + 0x6d2b79f5) >>> 0;
      let t = a;
      t = Math.imul(t ^ (t >>> 15), t | 1);
      t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
  }

  // ---------------------------------------------------------------- color

  const srgbToLinear = (u) => (u <= 0.04045 ? u / 12.92 : ((u + 0.055) / 1.055) ** 2.4);
  const linearToSrgb = (x) => (x <= 0.0031308 ? 12.92 * x : 1.055 * x ** (1 / 2.4) - 0.055);

  function hexToLinear(hex) {
    const n = parseInt(hex.slice(1), 16);
    return [(n >> 16) & 255, (n >> 8) & 255, n & 255].map((c) => srgbToLinear(c / 255));
  }

  function linearToHex(rgb) {
    return "#" + rgb.map((c) => Math.round(saturate(linearToSrgb(c)) * 255).toString(16).padStart(2, "0")).join("");
  }

  // narkowicz aces fit; same operator class as the engine composite
  const aces = (x) => saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
  const toDisplay = (c, exposure) => Math.round(linearToSrgb(aces(Math.max(0, c * exposure))) * 255);

  function hsl(h, s, l) {
    const k = (n) => (n + h / 30) % 12;
    const a = s * Math.min(l, 1 - l);
    const f = (n) => l - a * Math.max(-1, Math.min(k(n) - 3, Math.min(9 - k(n), 1)));
    return [f(0), f(8), f(4)];
  }

  // ------------------------------------------- shading (mirrors material.brdf / lighting.lights)

  const fresnelSchlick = (f0, vDotH) => {
    const x = (1 - vDotH) ** 5;
    return f0.map((f) => f + (1 - f) * x);
  };

  const ggxD = (nDotH, a2) => {
    const d = nDotH * nDotH * (a2 - 1) + 1;
    return a2 / (PI * d * d);
  };

  // height-correlated smith; contains 1 / (4 NdotL NdotV)
  const smithVisibility = (nDotL, nDotV, a2) => {
    const ggxV = nDotL * Math.sqrt(nDotV * nDotV * (1 - a2) + a2);
    const ggxL = nDotV * Math.sqrt(nDotL * nDotL * (1 - a2) + a2);
    return 0.5 / Math.max(ggxV + ggxL, 1e-7);
  };

  const materialInputs = (m) => {
    const alpha = Math.max(m.roughness * m.roughness, 1e-4);
    return {
      cDiff: scale(m.baseColor, 1 - m.metallic),
      f0: m.baseColor.map((c) => lerp(0.04, c, m.metallic)),
      a2: alpha * alpha,
    };
  };

  // f(v, l) without NdotL; caller multiplies radiance * NdotL once
  function evalBrdf(m, n, v, l) {
    const nDotL = dot(n, l);
    if (nDotL <= 0) return [0, 0, 0];
    const h = normalize(add(l, v));
    const nDotV = Math.max(dot(n, v), 1e-4);
    const nDotH = saturate(dot(n, h));
    const vDotH = saturate(dot(v, h));
    const { cDiff, f0, a2 } = materialInputs(m);
    const f = fresnelSchlick(f0, vDotH);
    const spec = ggxD(nDotH, a2) * smithVisibility(nDotL, nDotV, a2);
    return [0, 1, 2].map((i) => ((1 - f[i]) * cDiff[i]) / PI + f[i] * spec);
  }

  const rangeWindow = (d, range) => {
    if (range <= 0) return 1;
    const r = d / range;
    return saturate(1 - r * r * r * r);
  };

  const spotCone = (cosToAxis, inner, outer) => {
    const s = 1 / Math.max(0.001, Math.cos(inner) - Math.cos(outer));
    const t = saturate(cosToAxis * s - Math.cos(outer) * s);
    return t * t;
  };

  function evalLight(light, p) {
    if (light.type === "directional") {
      return { l: scale(light.worldDir, -1), radiance: scale(light.color, light.intensity), distance: Infinity };
    }
    const toLight = sub(light.worldPos, p);
    const d2 = Math.max(dot(toLight, toLight), 1e-8);
    const distance = Math.sqrt(d2);
    const l = scale(toLight, 1 / distance);
    let attenuation = rangeWindow(distance, light.range || 0) / Math.max(d2, 1e-4);
    if (light.type === "spot") attenuation *= spotCone(dot(light.worldDir, scale(l, -1)), light.inner, light.outer);
    return { l, radiance: scale(light.color, light.intensity * attenuation), distance };
  }

  // karis mobile env-brdf fit; pre-integrated specular albedo
  function envBrdfApprox(f0, roughness, nDotV) {
    const r = [roughness * -1 + 1, roughness * -0.0275 + 0.0425, roughness * -0.572 + 1.04, roughness * 0.022 - 0.04];
    const a004 = Math.min(r[0] * r[0], 2 ** (-9.28 * nDotV)) * r[0] + r[1];
    const ab = [-1.04 * a004 + r[2], 1.04 * a004 + r[3]];
    return f0.map((f) => f * ab[0] + ab[1]);
  }

  // ---------------------------------------------------------------- theme

  const css = (name) => getComputedStyle(root).getPropertyValue(name).trim();
  const palette = () => ({
    text: css("--text"),
    muted: css("--muted"),
    faint: css("--faint"),
    line: css("--line"),
    lineStrong: css("--line-strong"),
    accent: css("--accent"),
    accent2: css("--accent-2"),
    accent3: css("--accent-3"),
    accent4: css("--accent-4"),
    canvas: css("--canvas-bg"),
    surface: css("--surface"),
    ommOpaque: css("--omm-opaque"),
    ommTransparent: css("--omm-transparent"),
    ommUnknown: css("--omm-unknown"),
  });

  function readStoredTheme() {
    try {
      return localStorage.getItem("pbr-theme");
    } catch {
      return null;
    }
  }

  function storeTheme(value) {
    try {
      localStorage.setItem("pbr-theme", value);
    } catch {
      // storage blocked; theme still applies for this page view
    }
  }

  // ---------------------------------------------------------------- render scheduling

  // canvases render only while near the viewport; views may ask for more frames (accumulation)
  const views = new Set();
  let rafId = 0;

  const observer = new IntersectionObserver(
    (entries) => {
      for (const e of entries) {
        const view = e.target.__view;
        view.visible = e.isIntersecting;
        if (view.visible && view.dirty) kick();
      }
    },
    { rootMargin: "240px 0px" }
  );

  function kick() {
    if (!rafId) rafId = requestAnimationFrame(tick);
  }

  function tick() {
    rafId = 0;
    let pending = false;
    for (const view of views) {
      if (!view.visible || !view.dirty) continue;
      view.dirty = false;
      if (view.render()) view.dirty = true;
      pending ||= view.dirty;
    }
    if (pending) kick();
  }

  function createView(el, render) {
    const view = {
      visible: false,
      dirty: true,
      render,
      invalidate() {
        view.dirty = true;
        if (view.visible) kick();
      },
    };
    el.__view = view;
    views.add(view);
    observer.observe(el);
    return view;
  }

  const invalidateAll = () => views.forEach((v) => v.invalidate());

  function fitCanvas(canvas, maxDpr = 2) {
    const dpr = Math.min(window.devicePixelRatio || 1, maxDpr);
    const w = Math.max(1, Math.round(canvas.clientWidth * dpr));
    const h = Math.max(1, Math.round(canvas.clientHeight * dpr));
    if (canvas.width !== w || canvas.height !== h) {
      canvas.width = w;
      canvas.height = h;
    }
    const ctx = canvas.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    return { ctx, w: canvas.clientWidth, h: canvas.clientHeight };
  }

  // ---------------------------------------------------------------- controls

  const $ = (id) => document.getElementById(id);

  function formatValue(el) {
    const v = Number(el.value);
    if (el.dataset.fmt === "deg") return `${v}°`;
    const step = Number(el.step) || 1;
    return step >= 1 ? String(v) : v.toFixed(step < 0.1 ? 2 : 1);
  }

  function bindRange(id, onInput) {
    const el = $(id);
    const out = $(`${id}-val`);
    const sync = () => {
      if (out) out.textContent = formatValue(el);
    };
    el.addEventListener("input", () => {
      sync();
      onInput();
    });
    sync();
    return {
      el,
      sync,
      get value() {
        return Number(el.value);
      },
      set value(v) {
        el.value = String(v);
        sync();
      },
    };
  }

  // segmented control: exactly one aria-pressed button
  function bindSegment(container, attr, onChange) {
    const buttons = [...container.querySelectorAll(`button[data-${attr}]`)];
    const select = (value) => {
      buttons.forEach((b) => b.setAttribute("aria-pressed", String(b.dataset[attr] === value)));
      onChange(value);
    };
    buttons.forEach((b) => b.addEventListener("click", () => select(b.dataset[attr])));
    return {
      select,
      clear: () => buttons.forEach((b) => b.setAttribute("aria-pressed", "false")),
      get value() {
        return buttons.find((b) => b.getAttribute("aria-pressed") === "true")?.dataset[attr];
      },
    };
  }

  // ---------------------------------------------------------------- 2d plot

  class Plot {
    constructor(canvas, opts) {
      Object.assign(this, { x0: 0, x1: 1, y0: 0, y1: 1, xLabel: "", yLabel: "", logY: false }, opts);
      this.canvas = canvas;
      this.series = [];
      this.markers = [];
      this.hover = null;
      this.pad = { l: 50, r: 14, t: 14, b: 38 };
      const move = (e) => {
        const rect = canvas.getBoundingClientRect();
        this.hover = clamp(this.pxToX(e.clientX - rect.left), this.x0, this.x1);
        this.onHover?.(this.hover);
        this.view.invalidate();
      };
      canvas.addEventListener("pointermove", move);
      canvas.addEventListener("pointerdown", move);
      canvas.addEventListener("pointerleave", () => {
        this.hover = null;
        this.onHover?.(null);
        this.view.invalidate();
      });
    }

    pxToX(px) {
      return this.x0 + ((px - this.pad.l) / (this.w - this.pad.l - this.pad.r)) * (this.x1 - this.x0);
    }

    xToPx(x) {
      return this.pad.l + ((x - this.x0) / (this.x1 - this.x0)) * (this.w - this.pad.l - this.pad.r);
    }

    yToPx(y) {
      const map = (v) => (this.logY ? Math.log10(Math.max(v, 1e-6)) : v);
      const t = (map(y) - map(this.y0)) / (map(this.y1) - map(this.y0));
      return this.pad.t + (1 - t) * (this.h - this.pad.t - this.pad.b);
    }

    draw() {
      const { ctx, w, h } = fitCanvas(this.canvas);
      Object.assign(this, { w, h });
      const t = palette();
      const { l, r, t: pt, b } = this.pad;
      ctx.clearRect(0, 0, w, h);
      ctx.font = "11px system-ui, sans-serif";

      ctx.strokeStyle = t.line;
      ctx.lineWidth = 1;
      ctx.fillStyle = t.muted;
      ctx.textAlign = "right";
      ctx.textBaseline = "middle";
      for (let i = 0; i <= 4; i++) {
        const u = i / 4;
        const y = pt + (h - pt - b) * u;
        ctx.beginPath();
        ctx.moveTo(l, y);
        ctx.lineTo(w - r, y);
        ctx.stroke();
        const v = this.logY
          ? 10 ** lerp(Math.log10(this.y1), Math.log10(Math.max(this.y0, 1e-6)), u)
          : lerp(this.y1, this.y0, u);
        ctx.fillText(v >= 100 ? v.toFixed(0) : v >= 10 ? v.toFixed(1) : v.toFixed(2), l - 6, y);
      }
      ctx.textAlign = "center";
      ctx.textBaseline = "top";
      for (let i = 0; i <= 4; i++) {
        const x = lerp(this.x0, this.x1, i / 4);
        ctx.fillText(String(Math.round(x * 100) / 100), this.xToPx(x), h - b + 6);
      }
      ctx.fillText(this.xLabel, (l + w - r) / 2, h - 14);
      ctx.save();
      ctx.translate(12, (pt + h - b) / 2);
      ctx.rotate(-PI / 2);
      ctx.textBaseline = "middle";
      ctx.fillText(this.yLabel, 0, 0);
      ctx.restore();

      ctx.save();
      ctx.beginPath();
      ctx.rect(l, pt - 2, w - l - r, h - pt - b + 4);
      ctx.clip();
      for (const m of this.markers) {
        ctx.strokeStyle = m.color || t.faint;
        ctx.setLineDash([4, 4]);
        ctx.beginPath();
        ctx.moveTo(this.xToPx(m.x), pt);
        ctx.lineTo(this.xToPx(m.x), h - b);
        ctx.stroke();
        ctx.setLineDash([]);
      }
      for (const s of this.series) {
        ctx.beginPath();
        s.xs.forEach((x, i) => {
          const px = this.xToPx(x);
          const py = this.yToPx(s.ys[i]);
          if (i === 0) ctx.moveTo(px, py);
          else ctx.lineTo(px, py);
        });
        ctx.strokeStyle = s.color;
        ctx.lineWidth = s.width || 2.2;
        ctx.stroke();
      }
      if (this.hover != null) {
        const px = this.xToPx(this.hover);
        ctx.strokeStyle = t.text;
        ctx.globalAlpha = 0.3;
        ctx.beginPath();
        ctx.moveTo(px, pt);
        ctx.lineTo(px, h - b);
        ctx.stroke();
        ctx.globalAlpha = 1;
        for (const s of this.series) {
          ctx.fillStyle = s.color;
          ctx.beginPath();
          ctx.arc(px, this.yToPx(sampleSeries(s, this.hover)), 3.5, 0, 2 * PI);
          ctx.fill();
        }
      }
      ctx.restore();
    }
  }

  function sampleSeries({ xs, ys }, x) {
    if (x <= xs[0]) return ys[0];
    if (x >= xs[xs.length - 1]) return ys[ys.length - 1];
    let i = 1;
    while (xs[i] < x) i++;
    return lerp(ys[i - 1], ys[i], (x - xs[i - 1]) / (xs[i] - xs[i - 1]));
  }

  function makeSeries(from, to, n, fn) {
    const xs = [];
    const ys = [];
    for (let i = 0; i < n; i++) {
      const x = lerp(from, to, i / (n - 1));
      xs.push(x);
      ys.push(fn(x));
    }
    return { xs, ys };
  }

  function createPlot(canvasId, opts, build) {
    const canvas = $(canvasId);
    const plot = new Plot(canvas, opts);
    plot.view = createView(canvas, () => {
      build(plot, palette());
      plot.draw();
    });
    return plot;
  }

  // ---------------------------------------------------------------- §1 vectors

  function initVectors() {
    const svg = $("vec-svg");
    const readout = $("vec-read");
    const view = { invalidate: () => draw() };
    const lAngle = bindRange("vec-l", () => view.invalidate());
    const vAngle = bindRange("vec-v", () => view.invalidate());
    const plastic = { baseColor: [0.8, 0.8, 0.8], metallic: 0, roughness: 0.4 };
    const ox = 320;
    const oy = 205;
    const len = 150;
    const dir = (deg) => [Math.sin(deg * DEG), Math.cos(deg * DEG), 0];

    function arrow(v, color, label, dashed = false) {
      const x = ox + v[0] * len;
      const y = oy - v[1] * len;
      const ang = Math.atan2(y - oy, x - ox);
      const head = (a) => `${x - 10 * Math.cos(ang + a)},${y - 10 * Math.sin(ang + a)}`;
      const lx = ox + v[0] * (len + 20);
      const ly = oy - v[1] * (len + 20);
      return `<line x1="${ox}" y1="${oy}" x2="${x}" y2="${y}" stroke="${color}" stroke-width="2.4" ${dashed ? 'stroke-dasharray="5 4"' : ""}/>
        <polygon points="${x},${y} ${head(0.4)} ${head(-0.4)}" fill="${color}"/>
        <text x="${lx}" y="${ly}" text-anchor="middle" dominant-baseline="middle" style="fill:${color};font-weight:700;font-size:15px">${label}</text>`;
    }

    function draw() {
      const t = palette();
      const n = [0, 1, 0];
      const l = dir(lAngle.value);
      const v = dir(vAngle.value);
      const h = normalize(add(l, v));
      const hatch = Array.from({ length: 26 }, (_, i) => {
        const x = 60 + i * 20;
        return `<line x1="${x}" y1="${oy}" x2="${x - 12}" y2="${oy + 14}" stroke="${t.lineStrong}" />`;
      }).join("");
      svg.innerHTML = `
        <line x1="50" y1="${oy}" x2="590" y2="${oy}" stroke="${t.muted}" stroke-width="2"/>${hatch}
        ${arrow(n, t.text, "N")}
        ${arrow(h, t.accent2, "H", true)}
        ${arrow(l, t.accent, "L")}
        ${arrow(v, t.accent3, "V")}`;
      const nDotL = saturate(dot(n, l));
      const f = evalBrdf(plastic, n, v, l);
      const lo = luminance(f) * nDotL;
      readout.textContent =
        `NdotL=${nDotL.toFixed(3)}  NdotV=${saturate(dot(n, v)).toFixed(3)}  NdotH=${saturate(dot(n, h)).toFixed(3)}  ` +
        `VdotH=${saturate(dot(v, h)).toFixed(3)}  ·  white plastic r=0.4: f·NdotL=${lo.toFixed(3)}` +
        (nDotL <= 0 ? "  (light behind surface)" : "");
    }
    draw();
    return view;
  }

  // ---------------------------------------------------------------- §4–6, §10 plots

  function initFresnel() {
    const f0 = bindRange("fresnel-f0", () => plot.view.invalidate());
    const readout = $("read-fresnel");
    const schlick = (f, deg) => f + (1 - f) * (1 - Math.cos(deg * DEG)) ** 5;
    const plot = createPlot("plot-fresnel", { x1: 90, xLabel: "angle between V and H (°)", yLabel: "F" }, (p, t) => {
      p.series = [
        { ...makeSeries(0, 90, 181, (d) => schlick(0.04, d)), color: t.accent2 },
        { ...makeSeries(0, 90, 181, (d) => schlick(f0.value, d)), color: t.accent },
      ];
      p.markers = [{ x: 60 }, { x: 80 }];
      show(p.hover ?? 0);
    });
    function show(deg) {
      readout.textContent = `θ=${deg.toFixed(1)}°  VdotH=${Math.cos(deg * DEG).toFixed(3)}  F_dielectric=${schlick(0.04, deg).toFixed(3)}  F_metal=${schlick(f0.value, deg).toFixed(3)}`;
    }
  }

  function initAlpha() {
    const readout = $("read-alpha");
    const plot = createPlot("plot-alpha", { xLabel: "perceptual roughness", yLabel: "value" }, (p, t) => {
      p.series = [
        { ...makeSeries(0, 1, 60, (x) => x), color: t.faint, width: 1.5 },
        { ...makeSeries(0, 1, 60, (x) => x * x), color: t.accent },
        { ...makeSeries(0, 1, 60, (x) => x ** 4), color: t.accent2 },
      ];
    });
    plot.onHover = (r) => {
      readout.textContent = r == null ? "hover the plot" : `roughness=${r.toFixed(3)}  α=r²=${(r * r).toFixed(3)}  α²=r⁴=${(r ** 4).toFixed(4)}`;
    };
  }

  function initD() {
    const rough = bindRange("d-rough", () => plot.view.invalidate());
    const log = $("d-log");
    const readout = $("read-d");
    log.addEventListener("change", () => plot.view.invalidate());
    const a2Of = (r) => Math.max((r * r) ** 2, 1e-12);
    const plot = createPlot("plot-d", { xLabel: "N·H", yLabel: "D" }, (p, t) => {
      const a2 = a2Of(rough.value);
      const peak = 1 / (PI * a2);
      p.logY = log.checked;
      p.y0 = p.logY ? 0.01 : 0;
      p.y1 = p.logY ? Math.max(40, peak * 1.2) : clamp(peak * 1.05, 4, 40);
      p.series = [
        { ...makeSeries(0, 1, 240, (x) => ggxD(x, 1)), color: t.faint, width: 1.5 },
        { ...makeSeries(0, 1, 240, (x) => ggxD(x, 0.0625)), color: t.accent3, width: 1.5 },
        { ...makeSeries(0, 1, 400, (x) => ggxD(x, a2)), color: t.accent, width: 2.6 },
      ];
      const x = p.hover ?? 1;
      readout.textContent = `roughness=${rough.value.toFixed(2)}  α=${(rough.value ** 2).toFixed(4)}  N·H=${x.toFixed(3)}  D=${ggxD(x, a2).toFixed(3)}  peak=${peak.toFixed(1)}`;
    });
  }

  function initPolar() {
    const canvas = $("plot-polar");
    const rough = bindRange("polar-rough", () => view.invalidate());
    const view = createView(canvas, () => {
      const { ctx, w, h } = fitCanvas(canvas);
      const t = palette();
      ctx.clearRect(0, 0, w, h);
      const cx = w / 2;
      const cy = h - 26;
      const radius = Math.min(w / 2 - 20, h - 46);
      ctx.strokeStyle = t.line;
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.arc(cx, cy, radius, PI, 0);
      ctx.stroke();
      for (const a of [-60, -30, 0, 30, 60]) {
        ctx.beginPath();
        ctx.moveTo(cx, cy);
        ctx.lineTo(cx + Math.sin(a * DEG) * radius, cy - Math.cos(a * DEG) * radius);
        ctx.stroke();
      }
      ctx.strokeStyle = t.muted;
      ctx.beginPath();
      ctx.moveTo(cx - radius - 10, cy);
      ctx.lineTo(cx + radius + 10, cy);
      ctx.stroke();
      ctx.fillStyle = t.muted;
      ctx.font = "11px system-ui, sans-serif";
      ctx.textAlign = "center";
      ctx.fillText("N", cx, cy - radius - 8);

      const a2 = Math.max((rough.value * rough.value) ** 2, 1e-8);
      const peak = 1 / (PI * a2);
      ctx.beginPath();
      for (let i = 0; i <= 200; i++) {
        const theta = lerp(-PI / 2, PI / 2, i / 200);
        const rho = (ggxD(Math.cos(theta), a2) / peak) * radius;
        const x = cx + Math.sin(theta) * rho;
        const y = cy - Math.cos(theta) * rho;
        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }
      ctx.closePath();
      ctx.fillStyle = t.accent;
      ctx.globalAlpha = 0.16;
      ctx.fill();
      ctx.globalAlpha = 1;
      ctx.strokeStyle = t.accent;
      ctx.lineWidth = 2.4;
      ctx.stroke();
    });
  }

  function initRange() {
    const range = bindRange("light-range", () => plot.view.invalidate());
    const readout = $("read-range");
    const windowed = (d, r) => rangeWindow(d, r) / Math.max(d * d, 1e-4);
    const plot = createPlot("plot-range", { x0: 0.25, x1: 8, y1: 4, xLabel: "distance d (m)", yLabel: "attenuation" }, (p, t) => {
      p.x1 = Math.max(8, range.value * 1.15);
      p.series = [
        { ...makeSeries(p.x0, p.x1, 240, (d) => 1 / (d * d)), color: t.faint, width: 1.5 },
        { ...makeSeries(p.x0, p.x1, 320, (d) => windowed(d, range.value)), color: t.accent },
      ];
      p.markers = [{ x: range.value, color: t.accent }];
      const d = p.hover ?? 2;
      readout.textContent = `d=${d.toFixed(2)} m  1/d²=${(1 / (d * d)).toFixed(3)}  windowed=${windowed(d, range.value).toFixed(3)}`;
    });
  }

  function initSpotPlot() {
    const inner = bindRange("spot-inner", () => plot.view.invalidate());
    const outer = bindRange("spot-outer", () => plot.view.invalidate());
    const readout = $("read-spot");
    const plot = createPlot("plot-spot", { x1: 90, xLabel: "angle from spot axis (°)", yLabel: "cone" }, (p, t) => {
      if (inner.value >= outer.value) inner.value = Math.max(0, outer.value - 1);
      const i = inner.value * DEG;
      const o = outer.value * DEG;
      p.series = [{ ...makeSeries(0, 90, 361, (d) => spotCone(Math.cos(d * DEG), i, o)), color: t.accent }];
      p.markers = [{ x: inner.value, color: t.accent2 }, { x: outer.value, color: t.accent }];
      const d = p.hover ?? 0;
      readout.textContent = `θ=${d.toFixed(1)}°  cd=dot(spotDir,−L)=${Math.cos(d * DEG).toFixed(3)}  cone=${spotCone(Math.cos(d * DEG), i, o).toFixed(3)}`;
    });
  }

  // ---------------------------------------------------------------- §8 sphere

  function initSphere() {
    const canvas = $("plot-sphere");
    const metal = bindRange("mat-metal", manual);
    const rough = bindRange("mat-rough", manual);
    const expo = bindRange("mat-exp", () => view.invalidate());
    const color = $("mat-color");
    const stats = { cdiff: $("stat-cdiff"), f0: $("stat-f0"), a: $("stat-a"), lo: $("stat-lo") };
    let lightDir = normalize([0.45, 0.35, 0.82]);

    const presets = {
      plastic: { color: "#c8c8c8", metallic: 0, roughness: 0.4 },
      gold: { color: linearToHex([1.0, 0.766, 0.336]), metallic: 1, roughness: 0.15 },
      rubber: { color: "#1e1e1e", metallic: 0, roughness: 1 },
      default: { color: "#ffffff", metallic: 1, roughness: 1 },
    };
    const segment = bindSegment(canvas.closest(".fig").querySelector(".seg"), "preset", (name) => {
      const p = presets[name];
      color.value = p.color;
      metal.value = p.metallic;
      rough.value = p.roughness;
      view.invalidate();
    });
    function manual() {
      segment.clear();
      view.invalidate();
    }
    color.addEventListener("input", manual);

    const setLightFromPointer = (e) => {
      const rect = canvas.getBoundingClientRect();
      let u = ((e.clientX - rect.left) / rect.width - 0.5) / 0.42;
      let v = (0.5 - (e.clientY - rect.top) / rect.height) / 0.42;
      const r = Math.hypot(u, v);
      if (r > 0.999) {
        u /= r / 0.999;
        v /= r / 0.999;
      }
      lightDir = normalize([u, v, Math.sqrt(Math.max(0, 1 - u * u - v * v)) + 0.05]);
      view.invalidate();
    };
    canvas.addEventListener("pointerdown", (e) => {
      canvas.setPointerCapture(e.pointerId);
      setLightFromPointer(e);
    });
    canvas.addEventListener("pointermove", (e) => {
      if (canvas.hasPointerCapture(e.pointerId)) setLightFromPointer(e);
    });

    const view = createView(canvas, () => {
      const size = Math.round(Math.min(canvas.clientWidth, 440) * Math.min(window.devicePixelRatio || 1, 1.5));
      canvas.width = canvas.height = size;
      const ctx = canvas.getContext("2d");
      const img = ctx.createImageData(size, size);
      const data = img.data;
      const m = { baseColor: hexToLinear(color.value), metallic: metal.value, roughness: rough.value };
      const exposure = expo.value;
      const v = [0, 0, 1];
      const c = size / 2;
      const rad = size * 0.42;
      let peak = [0, 0, 0];
      for (let y = 0; y < size; y++) {
        for (let x = 0; x < size; x++) {
          const i = (y * size + x) * 4;
          const u = (x + 0.5 - c) / rad;
          const w = (c - y - 0.5) / rad;
          const r2 = u * u + w * w;
          data[i + 3] = 255;
          if (r2 > 1) {
            const bg = 10 + (y / size) * 12;
            data[i] = data[i + 1] = bg;
            data[i + 2] = bg + 3;
            continue;
          }
          const n = [u, w, Math.sqrt(1 - r2)];
          const lo = scale(evalBrdf(m, n, v, lightDir), Math.max(dot(n, lightDir), 0));
          if (luminance(lo) > luminance(peak)) peak = lo;
          data[i] = toDisplay(lo[0], exposure);
          data[i + 1] = toDisplay(lo[1], exposure);
          data[i + 2] = toDisplay(lo[2], exposure);
        }
      }
      ctx.putImageData(img, 0, 0);
      const { cDiff, f0 } = materialInputs(m);
      const fmt = (vec) => vec.map((x) => x.toFixed(3)).join(", ");
      stats.cdiff.textContent = fmt(cDiff);
      stats.f0.textContent = fmt(f0);
      stats.a.textContent = (m.roughness * m.roughness).toFixed(4);
      stats.lo.textContent = fmt(peak);
    });
  }

  // ---------------------------------------------------------------- scene tracer (§10 lab, §16 guides)

  const SCENE = {
    spheres: [
      { c: [-0.85, 0.55, 0], r: 0.55, mat: { baseColor: [0.62, 0.09, 0.06], metallic: 0, roughness: 0.85 } },
      { c: [0.85, 0.55, 0.15], r: 0.55, mat: { baseColor: [1.0, 0.766, 0.336], metallic: 1, roughness: 0.18 } },
    ],
    floorRadius: 5.5,
    floorTiles: [{ baseColor: [0.3, 0.28, 0.26], metallic: 0, roughness: 0.55 }, { baseColor: [0.22, 0.21, 0.2], metallic: 0, roughness: 0.55 }],
    camera: { pos: [0, 1.45, 4.4], target: [0, 0.45, 0], fovY: 38 * DEG },
    // dim studio dome for bounce rays that escape; camera misses use background()
    sky: (d) => scale([0.05, 0.055, 0.068], 0.25 + 0.75 * saturate(d[1])),
  };

  function intersectScene(o, d, tMax = Infinity) {
    let best = null;
    for (const s of SCENE.spheres) {
      const oc = sub(o, s.c);
      const b = dot(oc, d);
      const disc = b * b - (dot(oc, oc) - s.r * s.r);
      if (disc < 0) continue;
      const sq = Math.sqrt(disc);
      let t = -b - sq;
      if (t < 1e-4) t = -b + sq;
      if (t < 1e-4 || t >= tMax || (best && t >= best.t)) continue;
      const p = add(o, scale(d, t));
      best = { t, p, n: scale(sub(p, s.c), 1 / s.r), mat: s.mat };
    }
    if (d[1] < -1e-6) {
      const t = -o[1] / d[1];
      if (t > 1e-4 && t < tMax && (!best || t < best.t)) {
        const p = add(o, scale(d, t));
        if (p[0] * p[0] + p[2] * p[2] < SCENE.floorRadius * SCENE.floorRadius) {
          const tile = (Math.floor(p[0] * 2) + Math.floor(p[2] * 2)) & 1;
          best = { t, p, n: [0, 1, 0], mat: SCENE.floorTiles[tile] };
        }
      }
    }
    return best;
  }

  const occluded = (p, l, maxDist) => intersectScene(p, l, maxDist - 1e-3) !== null;

  function directLight(hit, v, light) {
    const s = evalLight(light, hit.p);
    const nDotL = dot(hit.n, s.l);
    if (nDotL <= 0 || luminance(s.radiance) <= 0) return [0, 0, 0];
    const origin = add(hit.p, scale(hit.n, 1e-3));
    if (occluded(origin, s.l, s.distance)) return [0, 0, 0];
    return mulv(evalBrdf(hit.mat, hit.n, v, s.l), scale(s.radiance, nDotL));
  }

  function tangentFrame(n) {
    const t = normalize(Math.abs(n[1]) < 0.99 ? cross([0, 1, 0], n) : cross([1, 0, 0], n));
    return [t, cross(n, t)];
  }

  // one stochastic bounce: cosine diffuse + ggx specular, each shaded with direct light
  function indirectLight(hit, v, light, rng) {
    const { cDiff, f0, a2 } = materialInputs(hit.mat);
    const [t, b] = tangentFrame(hit.n);
    const origin = add(hit.p, scale(hit.n, 1e-3));
    const bounce = (dir) => {
      const h2 = intersectScene(origin, dir);
      return h2 ? directLight(h2, scale(dir, -1), light) : SCENE.sky(dir);
    };
    let result = [0, 0, 0];

    if (luminance(cDiff) > 1e-4) {
      const u1 = rng();
      const phi = 2 * PI * rng();
      const r = Math.sqrt(u1);
      const dir = normalize(add(add(scale(t, r * Math.cos(phi)), scale(b, r * Math.sin(phi))), scale(hit.n, Math.sqrt(1 - u1))));
      result = add(result, mulv(cDiff, bounce(dir)));
    }

    const alpha = Math.sqrt(a2);
    const u1 = rng();
    const phi = 2 * PI * rng();
    const cosTheta = Math.sqrt((1 - u1) / (1 + (alpha * alpha - 1) * u1));
    const sinTheta = Math.sqrt(1 - cosTheta * cosTheta);
    const h = normalize(add(add(scale(t, sinTheta * Math.cos(phi)), scale(b, sinTheta * Math.sin(phi))), scale(hit.n, cosTheta)));
    const l = reflect(scale(v, -1), h);
    const nDotL = dot(hit.n, l);
    const nDotV = Math.max(dot(hit.n, v), 1e-4);
    if (nDotL > 0) {
      const vDotH = saturate(dot(v, h));
      const nDotH = Math.max(dot(hit.n, h), 1e-4);
      const f = fresnelSchlick(f0, vDotH);
      // ggx-sampled weight: F · G2 · VdotH / (NdotV · NdotH), G2 = 4 · V · NdotL · NdotV
      const weight = (4 * smithVisibility(nDotL, nDotV, a2) * nDotL * vDotH) / nDotH;
      result = add(result, mulv(scale(f, weight), bounce(l)));
    }
    return result;
  }

  const CAMERA_BASIS = (() => {
    const cam = SCENE.camera;
    const fwd = normalize(sub(cam.target, cam.pos));
    const right = normalize(cross(fwd, [0, 1, 0]));
    return { fwd, right, up: cross(right, fwd), tanY: Math.tan(cam.fovY / 2) };
  })();

  function cameraRay(w, h, x, y) {
    const { fwd, right, up, tanY } = CAMERA_BASIS;
    const px = ((2 * x) / w - 1) * tanY * (w / h);
    const py = (1 - (2 * y) / h) * tanY;
    return { o: SCENE.camera.pos, d: normalize(add(add(fwd, scale(right, px)), scale(up, py))), fwd };
  }

  class SceneTarget {
    constructor(canvas, width, height) {
      this.canvas = canvas;
      canvas.width = width;
      canvas.height = height;
      this.w = width;
      this.h = height;
      this.ctx = canvas.getContext("2d");
      this.image = this.ctx.createImageData(width, height);
      this.accum = new Float32Array(width * height * 3);
      this.frames = 0;
    }

    reset() {
      this.accum.fill(0);
      this.frames = 0;
    }

    // one jittered sample per pixel; returns false once `maxFrames` is reached
    accumulate(sample, maxFrames, exposure) {
      if (this.frames >= maxFrames) return false;
      const rng = mulberry32(0x9e3779b9 ^ (this.frames * 7919));
      const jx = this.frames === 0 ? 0.5 : rng();
      const jy = this.frames === 0 ? 0.5 : rng();
      const { w, h, accum } = this;
      for (let y = 0; y < h; y++) {
        for (let x = 0; x < w; x++) {
          const c = sample(x + jx, y + jy, rng);
          const i = (y * w + x) * 3;
          accum[i] += c[0];
          accum[i + 1] += c[1];
          accum[i + 2] += c[2];
        }
      }
      this.frames++;
      this.presentAccumulated(exposure);
      return this.frames < maxFrames;
    }

    presentAccumulated(exposure) {
      const { accum, frames } = this;
      if (frames === 0) return;
      this.present((i) => [accum[i] / frames, accum[i + 1] / frames, accum[i + 2] / frames], exposure);
    }

    present(read, exposure, raw = false) {
      const data = this.image.data;
      for (let p = 0, n = this.w * this.h; p < n; p++) {
        const c = read(p * 3);
        const o = p * 4;
        if (raw) {
          data[o] = Math.round(saturate(c[0]) * 255);
          data[o + 1] = Math.round(saturate(c[1]) * 255);
          data[o + 2] = Math.round(saturate(c[2]) * 255);
        } else {
          data[o] = toDisplay(c[0], exposure);
          data[o + 1] = toDisplay(c[1], exposure);
          data[o + 2] = toDisplay(c[2], exposure);
        }
        data[o + 3] = 255;
      }
      this.ctx.putImageData(this.image, 0, 0);
    }
  }

  function background(y, h) {
    const k = 0.004 + 0.01 * (1 - y / h);
    return [k, k, k * 1.25];
  }

  function shadePixel(target, x, y, light, withIndirect, rng) {
    const ray = cameraRay(target.w, target.h, x, y);
    const hit = intersectScene(ray.o, ray.d);
    if (!hit) return background(y, target.h);
    const v = scale(ray.d, -1);
    if (dot(hit.n, v) < 0) hit.n = scale(hit.n, -1);
    let c = directLight(hit, v, light);
    if (withIndirect) {
      const indirect = indirectLight(hit, v, light, rng);
      // firefly clamp; biased but display-only
      const lum = luminance(indirect);
      c = add(c, lum > 2 ? scale(indirect, 2 / lum) : indirect);
    }
    return c;
  }

  function makeLight(type, p) {
    if (type === "directional") {
      const toSun = [Math.cos(p.elev) * Math.sin(p.azim), Math.sin(p.elev), Math.cos(p.elev) * Math.cos(p.azim)];
      return { type, worldDir: scale(toSun, -1), color: [1, 0.96, 0.9], intensity: 2.6 };
    }
    if (type === "point") {
      return { type, worldPos: [p.px, p.py, 0.9], color: [1, 0.86, 0.66], intensity: 5, range: p.range };
    }
    const worldPos = [0, 3.1, 0.7];
    return {
      type,
      worldPos,
      worldDir: normalize(sub([p.aim, 0.2, 0], worldPos)),
      color: [1, 0.95, 0.86],
      intensity: 22,
      range: 0,
      inner: p.inner,
      outer: p.outer,
    };
  }

  const LAB_CAPTIONS = {
    directional: {
      photo: "pbr_figures/light-directional.jpg",
      text: "<b>Sun.</b> Parallel rays, so <code>L</code> is the same everywhere and nothing fades with distance. Shadows keep their width. <code>intensity</code> is lux.",
    },
    point: {
      photo: "pbr_figures/light-point.jpg",
      text: "<b>Point.</b> <code>L</code> changes per point. The closer sphere is brighter by 1/d². On gold the lamp shrinks to a small highlight. <code>intensity</code> is candela.",
    },
    spot: {
      photo: "pbr_figures/light-spot.jpg",
      text: "<b>Spot.</b> A point light limited to a cone: full inside <code>inner</code>, a soft ring out to <code>outer</code>, zero beyond. <code>cd = dot(spotDir, −L)</code>.",
    },
  };

  function initLightLab() {
    const fig = $("lab");
    const canvas = $("lab-canvas");
    const target = new SceneTarget(canvas, 512, 288);
    const photo = $("lab-photo");
    const caption = $("lab-caption");
    const readout = $("lab-read");
    const indirect = $("lab-indirect");
    let lightType = "directional";
    let previewShown = false;

    const restart = () => {
      target.reset();
      previewShown = false;
      view.invalidate();
    };
    const ranges = {
      elev: bindRange("lab-elev", restart),
      azim: bindRange("lab-azim", restart),
      px: bindRange("lab-px", restart),
      py: bindRange("lab-py", restart),
      range: bindRange("lab-range", restart),
      inner: bindRange("lab-inner", restart),
      outer: bindRange("lab-outer", restart),
      aim: bindRange("lab-aim", restart),
      // exposure is display-only; re-present the accumulated radiance
      exp: bindRange("lab-exp", () => target.presentAccumulated(ranges.exp.value)),
    };
    indirect.addEventListener("change", restart);

    bindSegment(fig.querySelector(".seg"), "light", (type) => {
      lightType = type;
      fig.querySelectorAll(".controls[data-for]").forEach((c) => (c.hidden = c.dataset.for !== type));
      photo.src = LAB_CAPTIONS[type].photo;
      caption.innerHTML = LAB_CAPTIONS[type].text;
      restart();
    });
    caption.innerHTML = LAB_CAPTIONS.directional.text;

    const currentLight = () => {
      if (ranges.inner.value >= ranges.outer.value) ranges.inner.value = Math.max(0, ranges.outer.value - 1);
      return makeLight(lightType, {
        elev: ranges.elev.value * DEG,
        azim: ranges.azim.value * DEG,
        px: ranges.px.value,
        py: ranges.py.value,
        range: ranges.range.value,
        inner: ranges.inner.value * DEG,
        outer: ranges.outer.value * DEG,
        aim: ranges.aim.value,
      });
    };

    const view = createView(canvas, () => {
      const light = currentLight();
      const withIndirect = indirect.checked;
      // direct-only preview keeps slider drags responsive; not accumulated
      if (withIndirect && target.frames === 0 && !previewShown) {
        previewShown = true;
        target.present((i) => {
          const p = i / 3;
          return shadePixel(target, (p % target.w) + 0.5, Math.floor(p / target.w) + 0.5, light, false, null);
        }, ranges.exp.value);
        describe(light);
        return true;
      }
      const more = target.accumulate((x, y, rng) => shadePixel(target, x, y, light, withIndirect, rng), withIndirect ? 96 : 8, ranges.exp.value);
      if (target.frames === 1) describe(light);
      return more;
    });

    // numbers at the top of the red sphere: the same terms as the light loop
    function describe(light) {
      const sphere = SCENE.spheres[0];
      const p = add(sphere.c, [0, sphere.r, 0]);
      const n = [0, 1, 0];
      const v = normalize(sub(SCENE.camera.pos, p));
      const s = evalLight(light, p);
      const nDotL = Math.max(dot(n, s.l), 0);
      const lo = luminance(mulv(evalBrdf(sphere.mat, n, v, s.l), scale(s.radiance, nDotL)));
      const extra = light.type === "spot" ? `  cd=${dot(light.worldDir, scale(s.l, -1)).toFixed(3)}` : light.type === "point" ? `  d=${s.distance.toFixed(2)} m` : "";
      readout.textContent = `top of red sphere: L_i=${luminance(s.radiance).toFixed(3)}  NdotL=${nDotL.toFixed(3)}  L_o=${lo.toFixed(3)} (unshadowed)${extra}`;
    }
  }

  function initGuides() {
    const fig = $("guides-canvas").closest(".fig");
    const canvas = $("guides-canvas");
    const target = new SceneTarget(canvas, 448, 252);
    const readout = $("guides-read");
    const light = makeLight("spot", { inner: 12 * DEG, outer: 30 * DEG, aim: 0 });
    let mode = "noisy";
    let noisySeed = 1;

    const notes = {
      noisy: "1 sample per pixel with one random bounce. This is what the path tracer hands DLSS-RR. Click again to re-roll.",
      converged: "Accumulating many frames of the same noisy signal. DLSS-RR aims for this from one frame plus the guides.",
      albedo: "c_diff = baseColor · (1 − metallic). The gold sphere is black here because metals have no diffuse.",
      specular: "Pre-integrated f0 at NdotV. Metals keep their color here, and dielectrics get a faint grey.",
      normal: "World shading normal as rgb = n · 0.5 + 0.5.",
      rough: "Perceptual roughness, not α. The gold is dark (0.18), rubber is bright (0.85).",
      depth: "Linear view depth, near = bright.",
    };

    bindSegment(fig.querySelector(".seg"), "guide", (m) => {
      if (m === "noisy" && mode === "noisy") noisySeed++;
      mode = m;
      target.reset();
      view.invalidate();
    });

    function guide(x, y) {
      const ray = cameraRay(target.w, target.h, x, y);
      const hit = intersectScene(ray.o, ray.d);
      if (!hit) return [0, 0, 0];
      const v = scale(ray.d, -1);
      const { cDiff, f0 } = materialInputs(hit.mat);
      switch (mode) {
        case "albedo":
          return cDiff.map(linearToSrgb);
        case "specular":
          return envBrdfApprox(f0, hit.mat.roughness, saturate(dot(hit.n, v))).map(linearToSrgb);
        case "normal":
          return hit.n.map((c) => c * 0.5 + 0.5);
        case "rough":
          return [hit.mat.roughness, hit.mat.roughness, hit.mat.roughness];
        default: {
          const depth = hit.t * dot(ray.d, ray.fwd);
          const k = saturate(1 - (depth - 2.5) / 5);
          return [k, k, k];
        }
      }
    }

    const view = createView(canvas, () => {
      readout.textContent = notes[mode];
      if (mode === "noisy") {
        const rng = mulberry32(noisySeed * 104729);
        target.present((i) => {
          const p = i / 3;
          return shadePixel(target, (p % target.w) + 0.5, Math.floor(p / target.w) + 0.5, light, true, rng);
        }, 1.3);
        return false;
      }
      if (mode === "converged") {
        return target.accumulate((x, y, rng) => shadePixel(target, x, y, light, true, rng), 128, 1.3);
      }
      target.present((i) => {
        const p = i / 3;
        return guide((p % target.w) + 0.5, Math.floor(p / target.w) + 0.5);
      }, 1, true);
      return false;
    });
  }

  // ---------------------------------------------------------------- §13 opacity micromaps

  function initOmm() {
    const canvas = $("omm-canvas");
    const fig = canvas.closest(".fig");
    const level = bindRange("omm-level", () => view.invalidate());
    const cutoff = bindRange("omm-cutoff", () => view.invalidate());
    let states = 4;
    bindSegment(fig.querySelector(".seg"), "omm", (s) => {
      states = Number(s);
      view.invalidate();
    });
    const stat = (id) => $(`omm-${id}`);

    // leaf alpha in uv space: pointed serrated blade along the diagonal + stem, soft edge
    function alpha(u, v) {
      const x = (u + v) / Math.SQRT2 / Math.SQRT2;
      const y = (v - u) / Math.SQRT2 - 0.05 * Math.sin(x * PI);
      if (x < 0.02 || x > 0.98) return 0;
      const t = (x - 0.18) / 0.8;
      const blade = t > 0 && t < 1 ? 0.3 * Math.sin(t * PI) ** 1.3 * (1 - 0.35 * t) + 0.012 * Math.sin(t * 60) : 0;
      const stem = x < 0.2 && Math.abs(y) < 0.014 ? 1 : 0;
      return Math.max(stem, saturate((blade - Math.abs(y)) * 90 + 0.5));
    }

    const tris = [
      [[0, 0], [1, 0], [0, 1]],
      [[1, 0], [1, 1], [0, 1]],
    ];
    const baryToUv = (t, b0, b1) => {
      const b2 = 1 - b0 - b1;
      return [t[0][0] * b2 + t[1][0] * b0 + t[2][0] * b1, t[0][1] * b2 + t[1][1] * b0 + t[2][1] * b1];
    };

    // conservative-ish classification from a 6×6 barycentric sample lattice
    function classify(tri, a, b, c, cut) {
      let above = 0;
      let below = 0;
      const n = 6;
      for (let i = 0; i <= n; i++) {
        for (let j = 0; j <= n - i; j++) {
          const w0 = i / n;
          const w1 = j / n;
          const w2 = 1 - w0 - w1;
          const b0 = a[0] * w2 + b[0] * w0 + c[0] * w1;
          const b1 = a[1] * w2 + b[1] * w0 + c[1] * w1;
          const [u, v] = baryToUv(tri, b0, b1);
          if (alpha(u, v) >= cut) above++;
          else below++;
        }
      }
      return below === 0 ? "opaque" : above === 0 ? "transparent" : "unknown";
    }

    let texture = null;
    function leafTexture(size, t) {
      if (texture && texture.size === size && texture.theme === t.canvas) return texture.canvas;
      const off = document.createElement("canvas");
      off.width = off.height = size;
      const ctx = off.getContext("2d");
      const img = ctx.createImageData(size, size);
      for (let y = 0; y < size; y++) {
        for (let x = 0; x < size; x++) {
          const a = alpha((x + 0.5) / size, 1 - (y + 0.5) / size);
          const checker = ((x >> 4) + (y >> 4)) & 1 ? 200 : 170;
          const leaf = [58, 128, 52];
          const o = (y * size + x) * 4;
          img.data[o] = lerp(checker, leaf[0], a);
          img.data[o + 1] = lerp(checker, leaf[1], a);
          img.data[o + 2] = lerp(checker, leaf[2], a);
          img.data[o + 3] = 255;
        }
      }
      ctx.putImageData(img, 0, 0);
      texture = { size, theme: t.canvas, canvas: off };
      return off;
    }

    const view = createView(canvas, () => {
      const { ctx, w } = fitCanvas(canvas);
      const t = palette();
      const pad = 14;
      const size = w - pad * 2;
      const texSize = Math.min(512, Math.round(size));
      ctx.clearRect(0, 0, w, w);
      ctx.imageSmoothingEnabled = true;
      ctx.drawImage(leafTexture(texSize, t), pad, pad, size, size);

      const cut = cutoff.value;
      const n = 2 ** level.value;
      const counts = { opaque: 0, transparent: 0, unknown: 0 };
      let wrongArea = 0;
      const toPx = (uv) => [pad + uv[0] * size, pad + (1 - uv[1]) * size];
      const colors = { opaque: t.ommOpaque, transparent: t.ommTransparent, unknown: t.ommUnknown };
      const drawMicro = (tri, a, b, c, state) => {
        const pts = [a, b, c].map((p) => toPx(baryToUv(tri, p[0], p[1])));
        ctx.beginPath();
        ctx.moveTo(pts[0][0], pts[0][1]);
        ctx.lineTo(pts[1][0], pts[1][1]);
        ctx.lineTo(pts[2][0], pts[2][1]);
        ctx.closePath();
        ctx.fillStyle = colors[state];
        ctx.globalAlpha = state === "transparent" ? 0.18 : state === "unknown" ? 0.62 : 0.42;
        ctx.fill();
        ctx.globalAlpha = n <= 16 ? 0.55 : 0.25;
        ctx.strokeStyle = t.surface;
        ctx.lineWidth = 0.7;
        ctx.stroke();
      };

      for (const tri of tris) {
        for (let i = 0; i < n; i++) {
          for (let j = 0; j < n - i; j++) {
            const up = [[i / n, j / n], [(i + 1) / n, j / n], [i / n, (j + 1) / n]];
            const micro = [up];
            if (j < n - 1 - i) micro.push([[(i + 1) / n, j / n], [(i + 1) / n, (j + 1) / n], [i / n, (j + 1) / n]]);
            for (const [a, b, c] of micro) {
              const state = classify(tri, a, b, c, cut);
              counts[state]++;
              if (state === "unknown" && states === 2) wrongArea++;
              drawMicro(tri, a, b, c, state === "unknown" && states === 2 ? "opaque" : state);
            }
          }
        }
      }

      ctx.globalAlpha = 1;
      ctx.strokeStyle = t.text;
      ctx.lineWidth = 1.5;
      ctx.strokeRect(pad, pad, size, size);
      ctx.beginPath();
      ctx.moveTo(pad + size, pad);
      ctx.lineTo(pad, pad + size);
      ctx.stroke();

      const total = counts.opaque + counts.transparent + counts.unknown;
      const pct = (x) => `${((100 * x) / total).toFixed(1)} %`;
      stat("total").textContent = `${total} (2 × 4^${level.value})`;
      stat("opaque").textContent = pct(counts.opaque);
      stat("transparent").textContent = pct(counts.transparent);
      stat("unknown").textContent = pct(counts.unknown);
      stat("anyhit").textContent = states === 4 ? `${pct(counts.unknown)} of hits` : "0 (none)";
      stat("error").textContent = states === 4 ? "0 (exact)" : `≤ ${pct(wrongArea)} of card`;
    });
  }

  // ---------------------------------------------------------------- §14 ReSTIR

  function initRestir() {
    const canvas = $("restir-canvas");
    const fig = canvas.closest(".fig");
    const W = 192;
    const H = 108;
    const WORLD_W = 16;
    const WORLD_H = 9;
    const LIGHTS = 300;
    const TEMPORAL_CAP = 20;
    const SPATIAL_TAPS = 5;
    const SPATIAL_RADIUS = 10;
    canvas.width = W;
    canvas.height = H;
    const ctx = canvas.getContext("2d");
    const image = ctx.createImageData(W, H);

    const rngScene = mulberry32(2024);
    const lights = Array.from({ length: LIGHTS }, (_, i) => {
      const bright = i < 8;
      const color = hsl(20 + rngScene() * 200, 0.45, 0.6);
      return {
        x: lerp(-0.5, WORLD_W + 0.5, rngScene()),
        z: lerp(-0.5, WORLD_H + 0.5, rngScene()),
        h: lerp(0.35, 1.1, rngScene()),
        power: scale(color, bright ? lerp(2, 4, rngScene()) : 0.05 + 0.6 * rngScene() ** 4),
      };
    });

    // presampling pdf ∝ power, like RTXDI's local-light pdf texture
    const cdf = new Float32Array(LIGHTS);
    let totalPower = 0;
    lights.forEach((l, i) => {
      totalPower += luminance(l.power);
      cdf[i] = totalPower;
    });
    const sourcePdf = (li) => luminance(lights[li].power) / totalPower;
    const sampleByPower = () => {
      const u = rng() * totalPower;
      let lo = 0;
      let hi = LIGHTS - 1;
      while (lo < hi) {
        const mid = (lo + hi) >> 1;
        if (cdf[mid] < u) lo = mid + 1;
        else hi = mid;
      }
      return lo;
    };

    const px = new Float32Array(W * H * 2);
    for (let y = 0; y < H; y++) {
      for (let x = 0; x < W; x++) {
        px[(y * W + x) * 2] = ((x + 0.5) / W) * WORLD_W;
        px[(y * W + x) * 2 + 1] = ((y + 0.5) / H) * WORLD_H;
      }
    }

    // lambert floor under a point light: I · cosθ / d², cosθ = h / d
    const contribution = (p, li, out) => {
      const L = lights[li];
      const dx = L.x - px[p * 2];
      const dz = L.z - px[p * 2 + 1];
      const d2 = dx * dx + dz * dz + L.h * L.h;
      const k = L.h / (d2 * Math.sqrt(d2));
      out[0] = L.power[0] * k;
      out[1] = L.power[1] * k;
      out[2] = L.power[2] * k;
      return out;
    };
    const tmp = [0, 0, 0];
    const targetPdf = (p, li) => luminance(contribution(p, li, tmp));

    const reference = new Float32Array(W * H * 3);
    for (let p = 0; p < W * H; p++) {
      for (let li = 0; li < LIGHTS; li++) {
        contribution(p, li, tmp);
        reference[p * 3] += tmp[0];
        reference[p * 3 + 1] += tmp[1];
        reference[p * 3 + 2] += tmp[2];
      }
    }
    let meanLum = 0;
    for (let p = 0; p < W * H; p++) meanLum += luminance([reference[p * 3], reference[p * 3 + 1], reference[p * 3 + 2]]);
    meanLum /= W * H;
    const exposure = 0.25 / meanLum;
    const AUTO_FRAMES = 30;

    // reservoir soa: y (light), W (contribution weight), M (sample count)
    const makeReservoirs = () => ({ y: new Int32Array(W * H).fill(-1), W: new Float32Array(W * H), M: new Float32Array(W * H) });
    let history = makeReservoirs();
    let current = makeReservoirs();
    let spatialOut = makeReservoirs();
    let frame = 0;
    let rng = mulberry32(1);
    const estimate = new Float32Array(W * H * 3);

    const m = bindRange("restir-m", reset);
    const spatial = $("restir-spatial");
    const temporal = $("restir-temporal");
    const play = $("restir-play");
    spatial.addEventListener("change", reset);
    temporal.addEventListener("change", reset);
    let mode = "ris";
    let playing = false;
    bindSegment(fig.querySelector(".seg"), "restir", (value) => {
      mode = value;
      reset();
    });
    play.addEventListener("click", () => {
      playing = !playing;
      play.setAttribute("aria-pressed", String(playing));
      play.textContent = playing ? "Pause" : "Play frames";
      view.invalidate();
    });

    function reset() {
      history = makeReservoirs();
      frame = 0;
      rng = mulberry32(1);
      view.invalidate();
    }

    // streaming combine of (candidate, weight, count) into a running reservoir
    function stream(state, y, weight, count) {
      state.wSum += weight;
      state.M += count;
      if (weight > 0 && rng() * state.wSum < weight) state.y = y;
    }

    function finalize(res, p, state) {
      res.y[p] = state.y;
      res.M[p] = state.M;
      const pHat = state.y >= 0 ? targetPdf(p, state.y) : 0;
      res.W[p] = pHat > 0 ? state.wSum / (state.M * pHat) : 0;
    }

    function stepRis() {
      const M = m.value;
      const state = { y: -1, wSum: 0, M: 0 };
      for (let p = 0; p < W * H; p++) {
        state.y = -1;
        state.wSum = 0;
        state.M = 0;
        for (let j = 0; j < M; j++) {
          const li = sampleByPower();
          stream(state, li, targetPdf(p, li) / sourcePdf(li), 1);
        }
        if (temporal.checked && history.y[p] >= 0) {
          const hM = Math.min(history.M[p], TEMPORAL_CAP * M);
          stream(state, history.y[p], targetPdf(p, history.y[p]) * history.W[p] * hM, hM);
        }
        finalize(current, p, state);
      }

      let result = current;
      if (spatial.checked) {
        for (let p = 0; p < W * H; p++) {
          state.y = current.y[p];
          state.M = current.M[p];
          state.wSum = state.y >= 0 ? targetPdf(p, state.y) * current.W[p] * current.M[p] : 0;
          const x = p % W;
          const y = Math.floor(p / W);
          for (let k = 0; k < SPATIAL_TAPS; k++) {
            const a = rng() * 2 * PI;
            const r = Math.sqrt(rng()) * SPATIAL_RADIUS;
            const nx = clamp(Math.round(x + Math.cos(a) * r), 0, W - 1);
            const ny = clamp(Math.round(y + Math.sin(a) * r), 0, H - 1);
            const q = ny * W + nx;
            if (current.y[q] < 0) continue;
            stream(state, current.y[q], targetPdf(p, current.y[q]) * current.W[q] * current.M[q], current.M[q]);
          }
          finalize(spatialOut, p, state);
        }
        result = spatialOut;
      }

      for (let p = 0; p < W * H; p++) {
        const y = result.y[p];
        if (y < 0) {
          estimate[p * 3] = estimate[p * 3 + 1] = estimate[p * 3 + 2] = 0;
          continue;
        }
        contribution(p, y, tmp);
        estimate[p * 3] = tmp[0] * result.W[p];
        estimate[p * 3 + 1] = tmp[1] * result.W[p];
        estimate[p * 3 + 2] = tmp[2] * result.W[p];
      }
      // the shaded reservoir feeds next frame's temporal pass; recycle the old history buffer
      const recycled = history;
      history = result;
      if (result === spatialOut) spatialOut = recycled;
      else current = recycled;
    }

    function stepUniform() {
      for (let p = 0; p < W * H; p++) {
        contribution(p, Math.floor(rng() * LIGHTS), tmp);
        estimate[p * 3] = tmp[0] * LIGHTS;
        estimate[p * 3 + 1] = tmp[1] * LIGHTS;
        estimate[p * 3 + 2] = tmp[2] * LIGHTS;
      }
    }

    const view = createView(canvas, () => {
      if (mode === "reference") estimate.set(reference);
      else if (mode === "uniform") stepUniform();
      else stepRis();
      frame++;

      let err = 0;
      for (let p = 0; p < W * H; p++) {
        const o = p * 4;
        const i = p * 3;
        image.data[o] = toDisplay(estimate[i], exposure);
        image.data[o + 1] = toDisplay(estimate[i + 1], exposure);
        image.data[o + 2] = toDisplay(estimate[i + 2], exposure);
        image.data[o + 3] = 255;
        const d = luminance([estimate[i], estimate[i + 1], estimate[i + 2]]) - luminance([reference[i], reference[i + 1], reference[i + 2]]);
        err += d * d;
      }
      ctx.putImageData(image, 0, 0);

      const rmse = Math.sqrt(err / (W * H)) / meanLum;
      const evals = mode === "reference" ? LIGHTS : mode === "uniform" ? 1 : m.value + (temporal.checked ? 1 : 0) + (spatial.checked ? SPATIAL_TAPS : 0);
      $("restir-evals").textContent = String(evals);
      $("restir-rays").textContent = mode === "reference" ? `${LIGHTS}` : "1";
      $("restir-rmse").textContent = `${(rmse * 100).toFixed(1)} %`;
      $("restir-frame").textContent = mode === "reference" ? "—" : String(frame);

      if (playing && mode !== "reference" && frame >= 240) {
        playing = false;
        play.setAttribute("aria-pressed", "false");
        play.textContent = "Play frames";
      }
      // ReSTIR auto-runs a short history so reuse is visible without pressing play
      return mode !== "reference" && (playing || (mode === "ris" && frame < AUTO_FRAMES));
    });
  }

  // ---------------------------------------------------------------- chrome: theme, toc, progress

  function initTheme() {
    const saved = readStoredTheme();
    if (saved === "light" || saved === "dark") root.dataset.theme = saved;
    const system = window.matchMedia("(prefers-color-scheme: dark)");
    const effective = () => root.dataset.theme || (system.matches ? "dark" : "light");
    $("theme-toggle").addEventListener("click", () => {
      const next = effective() === "dark" ? "light" : "dark";
      root.dataset.theme = next;
      storeTheme(next);
      onThemeChange();
    });
    system.addEventListener("change", onThemeChange);
  }

  let themeListeners = [];
  const onThemeChange = () => {
    themeListeners.forEach((fn) => fn());
    invalidateAll();
  };

  function initToc() {
    const toc = $("toc");
    if (window.matchMedia("(max-width: 1040px)").matches) toc.open = false;
    const links = [...toc.querySelectorAll("a")];
    const byId = new Map(links.map((a) => [a.getAttribute("href").slice(1), a]));
    const spy = new IntersectionObserver(
      (entries) => {
        const hit = entries.filter((e) => e.isIntersecting).sort((a, b) => a.boundingClientRect.top - b.boundingClientRect.top)[0];
        if (!hit) return;
        links.forEach((a) => a.classList.remove("active"));
        byId.get(hit.target.id)?.classList.add("active");
      },
      { rootMargin: "-15% 0px -70% 0px" }
    );
    byId.forEach((_, id) => {
      const section = $(id);
      if (section) spy.observe(section);
    });
    toc.addEventListener("click", (e) => {
      if (e.target.closest("a") && window.matchMedia("(max-width: 1040px)").matches) toc.open = false;
    });

    const bar = $("progress");
    const update = () => {
      const max = document.documentElement.scrollHeight - window.innerHeight;
      bar.style.width = `${max > 0 ? (100 * window.scrollY) / max : 0}%`;
    };
    window.addEventListener("scroll", update, { passive: true });
    update();
  }

  function init() {
    initTheme();
    initToc();
    const vectors = initVectors();
    themeListeners.push(() => vectors.invalidate());
    initFresnel();
    initAlpha();
    initD();
    initPolar();
    initSphere();
    initLightLab();
    initRange();
    initSpotPlot();
    initOmm();
    initRestir();
    initGuides();
    let resizeTimer = 0;
    window.addEventListener("resize", () => {
      clearTimeout(resizeTimer);
      resizeTimer = setTimeout(invalidateAll, 120);
    });
  }

  document.addEventListener("DOMContentLoaded", init);
})();
