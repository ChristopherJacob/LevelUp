/**
 * LevelUp Bubble Card — Canvas-based spirit level for Home Assistant
 * 
 * Usage: type: custom:levelup-bubble-card
 *   roll_entity: sensor.levelup_roll
 *   pitch_entity: sensor.levelup_pitch
 *   tolerance: 1.5
 */
class LevelUpBubbleCard extends HTMLElement {

  static getStubConfig() {
    return { roll_entity: 'sensor.levelup_roll', pitch_entity: 'sensor.levelup_pitch' };
  }

  setConfig(c) {
    if (!c.roll_entity || !c.pitch_entity) throw new Error('roll_entity and pitch_entity required');
    this._cfg = Object.assign({ tolerance: 1.5, title: '', multiplier: 4, size: 240 }, c);
  }

  set hass(h) {
    if (!this._built) { this._build(); this._built = true; }
    const r = parseFloat(h.states[this._cfg.roll_entity]?.state) || 0;
    const p = parseFloat(h.states[this._cfg.pitch_entity]?.state) || 0;
    this._draw(r, p);
  }

  _build() {
    const { title: t, size: s } = this._cfg;
    this.innerHTML = `<ha-card>
      <div id="lbc-root" style="display:flex;flex-direction:column;align-items:center;padding:16px 8px;gap:8px;background:var(--card-background-color,#1c1c1e);border-radius:12px;">
        ${t?`<div style="font-size:13px;font-weight:600;color:var(--secondary-text-color,#888);text-transform:uppercase;letter-spacing:0.5px">${t}</div>`:''}
        <canvas id="lbc-canvas" width="${s}" height="${s}" style="display:block"></canvas>
        <div id="lbc-status" style="font-size:12px;font-weight:600;color:#888;letter-spacing:0.5px">--</div>
        <div style="display:flex;gap:20px">
          <div style="display:flex;flex-direction:column;align-items:center">
            <span style="font-size:9px;color:#666;text-transform:uppercase">Roll</span>
            <span id="lbc-roll" style="font-family:monospace;font-size:16px;font-weight:500;color:var(--primary-text-color,#ccc)">--°</span>
          </div>
          <div style="display:flex;flex-direction:column;align-items:center">
            <span style="font-size:9px;color:#666;text-transform:uppercase">Pitch</span>
            <span id="lbc-pitch" style="font-family:monospace;font-size:16px;font-weight:500;color:var(--primary-text-color,#ccc)">--°</span>
          </div>
        </div>
      </div>
    </ha-card>`;
    this._draw(0, 0);
  }

  _draw(roll, pitch) {
    const c = this.querySelector('#lbc-canvas');
    if (!c) return;
    const ctx = c.getContext('2d');
    const { size: s, tolerance: tol, multiplier: mul } = this._cfg;
    const cx = s/2, cy = s/2, ro = s*0.44, ri = s*0.40;
    const isLevel = Math.abs(roll) <= tol && Math.abs(pitch) <= tol;

    // Clear
    ctx.clearRect(0, 0, s, s);

    // Outer ring
    ctx.beginPath();
    ctx.arc(cx, cy, ro, 0, Math.PI*2);
    ctx.strokeStyle = '#3a3a3c';
    ctx.lineWidth = 2;
    ctx.stroke();

    // Inner ring
    ctx.beginPath();
    ctx.arc(cx, cy, ri, 0, Math.PI*2);
    ctx.strokeStyle = '#2c2c2e';
    ctx.lineWidth = 1;
    ctx.stroke();

    // Crosshair
    ctx.strokeStyle = '#2c2c2e';
    ctx.lineWidth = 0.5;
    ctx.beginPath(); ctx.moveTo(cx, cy-ri-3); ctx.lineTo(cx, cy+ri+3); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(cx-ri-3, cy); ctx.lineTo(cx+ri+3, cy); ctx.stroke();

    // Tick marks
    ctx.strokeStyle = '#3a3a3c';
    ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(cx, cy-ro); ctx.lineTo(cx, cy-ro+7); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(cx, cy+ro); ctx.lineTo(cx, cy+ro-7); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(cx-ro, cy); ctx.lineTo(cx-ro+7, cy); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(cx+ro, cy); ctx.lineTo(cx+ro-7, cy); ctx.stroke();

    // Center dot
    ctx.beginPath();
    ctx.arc(cx, cy, 2.5, 0, Math.PI*2);
    ctx.fillStyle = '#555';
    ctx.fill();

    // Bubble position
    const sc = 2.5 * mul;
    const max = ri - 16;
    const bx = cx + Math.max(-max, Math.min(max, roll * sc));
    const by = cy + Math.max(-max, Math.min(max, -pitch * sc));

    // Bubble glow
    const grd = ctx.createRadialGradient(bx, by, 2, bx, by, 26);
    if (isLevel) {
      grd.addColorStop(0, 'rgba(52,199,89,0.6)');
      grd.addColorStop(0.5, 'rgba(52,199,89,0.15)');
      grd.addColorStop(1, 'rgba(52,199,89,0)');
    } else {
      grd.addColorStop(0, 'rgba(31,111,235,0.5)');
      grd.addColorStop(0.5, 'rgba(31,111,235,0.1)');
      grd.addColorStop(1, 'rgba(31,111,235,0)');
    }
    ctx.beginPath();
    ctx.arc(bx, by, 26, 0, Math.PI*2);
    ctx.fillStyle = grd;
    ctx.fill();

    // Bubble
    ctx.beginPath();
    ctx.arc(bx, by, isLevel ? 18 : 16, 0, Math.PI*2);
    ctx.fillStyle = isLevel ? '#34c759' : '#1f6feb';
    ctx.fill();

    // Bubble highlight
    ctx.beginPath();
    ctx.arc(bx-5, by-5, 5, 0, Math.PI*2);
    ctx.fillStyle = 'rgba(255,255,255,0.3)';
    ctx.fill();

    // Status text
    const st = this.querySelector('#lbc-status');
    const rv = this.querySelector('#lbc-roll');
    const pv = this.querySelector('#lbc-pitch');
    if (st) {
      st.textContent = isLevel ? '● LEVEL' : `○ ${Math.max(Math.abs(roll), Math.abs(pitch)).toFixed(1)}° off`;
      st.style.color = isLevel ? '#34c759' : '#888';
    }
    if (rv) rv.textContent = roll.toFixed(1) + '°';
    if (pv) pv.textContent = pitch.toFixed(1) + '°';
  }

  getCardSize() { return 3; }
}
customElements.define('levelup-bubble-card', LevelUpBubbleCard);
