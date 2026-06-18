/**
 * LevelUp Bubble Card — Polished spirit-level display for Home Assistant
 */
class LevelUpBubbleCard extends HTMLElement {

  static getStubConfig() {
    return { roll_entity: 'sensor.levelup_roll', pitch_entity: 'sensor.levelup_pitch' };
  }

  setConfig(c) {
    if (!c.roll_entity || !c.pitch_entity) throw new Error('roll_entity and pitch_entity required');
    this._cfg = Object.assign({ tolerance: 1.5, title: '', multiplier: 4, size: 260 }, c);
    this._tx = 0; this._ty = 0;
  }

  set hass(h) {
    if (!this._built) { this._build(); this._built = true; }
    const r = parseFloat(h.states[this._cfg.roll_entity]?.state) || 0;
    const p = parseFloat(h.states[this._cfg.pitch_entity]?.state) || 0;
    this._tx = r; this._ty = p;
    this._tick();
  }

  _build() {
    const t = this._cfg.title;
    this.innerHTML = '<ha-card><div style="display:flex;flex-direction:column;align-items:center;padding:20px 12px;gap:10px;background:var(--card-background-color,#1C1F26);border-radius:16px">'
      + (t ? '<div style="font-size:13px;font-weight:600;color:var(--secondary-text-color,#D6C7A1);text-transform:uppercase;letter-spacing:1px">' + t + '</div>' : '')
      + '<canvas id="lc" style="display:block;border-radius:50%"></canvas>'
      + '<div id="ls" style="font-size:13px;font-weight:600;color:var(--secondary-text-color,#D6C7A1);letter-spacing:0.5px">--</div>'
      + '<div style="display:flex;gap:24px">'
      + '<div style="display:flex;flex-direction:column;align-items:center"><span style="font-size:9px;color:var(--secondary-text-color,#D6C7A1);opacity:0.5;text-transform:uppercase;letter-spacing:0.5px">Roll</span><span id="lr" style="font-family:SF Mono,monospace;font-size:16px;font-weight:500;color:var(--primary-text-color,#FEF5E6)">--°</span></div>'
      + '<div style="display:flex;flex-direction:column;align-items:center"><span style="font-size:9px;color:var(--secondary-text-color,#D6C7A1);opacity:0.5;text-transform:uppercase;letter-spacing:0.5px">Pitch</span><span id="lp" style="font-family:SF Mono,monospace;font-size:16px;font-weight:500;color:var(--primary-text-color,#FEF5E6)">--°</span></div>'
      + '</div></div></ha-card>';
    this._anim = null;
    this._tick();
  }

  _tick() {
    if (this._anim) return;
    this._anim = requestAnimationFrame(() => {
      this._anim = null;
      this._draw(this._tx, this._ty);
    });
  }

  _draw(roll, pitch) {
    const c = this.querySelector('#lc');
    if (!c) return;
    try {
      const s = this._cfg.size, pr = window.devicePixelRatio || 1;
      c.width = s * pr; c.height = s * pr;
      c.style.width = s + 'px'; c.style.height = s + 'px';
      const ctx = c.getContext('2d');
      ctx.scale(pr, pr);

      const cx = s/2, cy = s/2;
      const ro = s*0.425, ri = s*0.38, rbub = 15;
      const lvl = Math.abs(roll) <= this._cfg.tolerance && Math.abs(pitch) <= this._cfg.tolerance;
      const C = { bg:'#1C1F26', ring:'#2A2D35', tick:'#3D4150', hl:'#D6C7A1', green:'#6E7F5F', blue:'#4A90D9', text:'#FEF5E6', white:'rgba(255,255,255,0.12)' };

      // Clear
      ctx.clearRect(0, 0, s, s);

      // Background circle
      ctx.beginPath(); ctx.arc(cx, cy, ro+12, 0, Math.PI*2);
      ctx.fillStyle = C.ring; ctx.fill();

      // Tick ring
      ctx.beginPath(); ctx.arc(cx, cy, ro, 0, Math.PI*2);
      ctx.strokeStyle = C.tick; ctx.lineWidth = 1.5; ctx.stroke();

      // Inner ring (subtle)
      ctx.beginPath(); ctx.arc(cx, cy, ri, 0, Math.PI*2);
      ctx.strokeStyle = 'rgba(255,255,255,0.06)'; ctx.lineWidth = 1; ctx.stroke();

      // Degree ticks every 2 degrees
      for (let a = 0; a < 360; a += 2) {
        const rad = a * Math.PI / 180;
        const major = a % 10 === 0;
        const r1 = ro - (major ? 6 : 3);
        const r2 = ro + (major ? 0 : 0);
        ctx.beginPath();
        ctx.moveTo(cx + Math.cos(rad) * r1, cy + Math.sin(rad) * r1);
        ctx.lineTo(cx + Math.cos(rad) * r2, cy + Math.sin(rad) * r2);
        ctx.strokeStyle = major ? C.hl : C.tick;
        ctx.lineWidth = major ? 1 : 0.5;
        ctx.stroke();
      }

      // Crosshair (faint)
      ctx.strokeStyle = C.white; ctx.lineWidth = 0.5;
      ctx.beginPath(); ctx.moveTo(cx, cy-ri+4); ctx.lineTo(cx, cy+ri-4); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(cx-ri+4, cy); ctx.lineTo(cx+ri-4, cy); ctx.stroke();

      // Center target
      ctx.beginPath(); ctx.arc(cx, cy, 3.5, 0, Math.PI*2);
      ctx.strokeStyle = C.hl; ctx.lineWidth = 1; ctx.stroke();
      ctx.beginPath(); ctx.arc(cx, cy, 1, 0, Math.PI*2);
      ctx.fillStyle = C.hl; ctx.fill();

      // Bubble position
      const sc = 2.5 * this._cfg.multiplier;
      const max = ri - rbub - 4;
      const bx = cx + Math.max(-max, Math.min(max, roll * sc));
      const by = cy + Math.max(-max, Math.min(max, -pitch * sc));

      // Bubble shadow
      ctx.beginPath(); ctx.arc(bx+2, by+2, rbub+2, 0, Math.PI*2);
      ctx.fillStyle = 'rgba(0,0,0,0.25)'; ctx.fill();

      // Bubble glow
      const glow = ctx.createRadialGradient(bx, by, 4, bx, by, rbub+10);
      if (lvl) {
        glow.addColorStop(0, 'rgba(110,127,95,0.35)');
        glow.addColorStop(0.4, 'rgba(110,127,95,0.1)');
        glow.addColorStop(1, 'rgba(110,127,95,0)');
      } else {
        glow.addColorStop(0, 'rgba(74,144,217,0.3)');
        glow.addColorStop(0.4, 'rgba(74,144,217,0.08)');
        glow.addColorStop(1, 'rgba(74,144,217,0)');
      }
      ctx.beginPath(); ctx.arc(bx, by, rbub+10, 0, Math.PI*2);
      ctx.fillStyle = glow; ctx.fill();

      // Bubble body
      const body = ctx.createRadialGradient(bx-4, by-6, 1, bx, by, rbub);
      if (lvl) {
        body.addColorStop(0, '#8FAF7E');
        body.addColorStop(0.6, '#6E7F5F');
        body.addColorStop(1, '#4A5A3F');
      } else {
        body.addColorStop(0, '#7AB8F5');
        body.addColorStop(0.6, '#4A90D9');
        body.addColorStop(1, '#2A5A8A');
      }
      ctx.beginPath(); ctx.arc(bx, by, rbub, 0, Math.PI*2);
      ctx.fillStyle = body; ctx.fill();

      // Bubble glass highlight
      ctx.beginPath(); ctx.ellipse(bx-5, by-7, 7, 4, -0.3, 0, Math.PI*2);
      ctx.fillStyle = 'rgba(255,255,255,0.25)'; ctx.fill();

      // Tiny specular
      ctx.beginPath(); ctx.arc(bx-6, by-8, 2.5, 0, Math.PI*2);
      ctx.fillStyle = 'rgba(255,255,255,0.6)'; ctx.fill();

      // Status
      const st = this.querySelector('#ls');
      if (st) {
        const off = Math.max(Math.abs(roll), Math.abs(pitch));
        st.textContent = lvl ? '⚫ LEVEL' : '⚪ ' + off.toFixed(1) + '° off';
        st.style.color = lvl ? '#6E7F5F' : '#D6C7A1';
      }
      const rv = this.querySelector('#lr'), pv = this.querySelector('#lp');
      if (rv) rv.textContent = roll.toFixed(1) + '°';
      if (pv) pv.textContent = pitch.toFixed(1) + '°';

    } catch(e) {
      console.error('LevelUp draw error:', e);
    }
  }

  disconnectedCallback() {
    if (this._anim) { cancelAnimationFrame(this._anim); this._anim = null; }
  }

  getCardSize() { return 3; }
}
console.log('🔵 LevelUp Bubble Card v2 registered');
customElements.define('levelup-bubble-card', LevelUpBubbleCard);
