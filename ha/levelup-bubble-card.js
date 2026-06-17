/**
 * LevelUp Bubble Card — A clean, modern spirit-level display for Home Assistant
 * 
 * Installation:
 *   1. Copy this file to /config/www/levelup-bubble-card.js
 *   2. Add resource: /local/levelup-bubble-card.js (JavaScript Module)
 *   3. Use in dashboard:
 *      type: custom:levelup-bubble-card
 *      roll_entity: sensor.levelup_roll
 *      pitch_entity: sensor.levelup_pitch
 *      tolerance: 1.5
 *      title: Van Level
 * 
 * Requires: LevelUp sensors publishing via MQTT (roll_deg, pitch_deg)
 */

class LevelUpBubbleCard extends HTMLElement {
  static getConfigElement() {
    return document.createElement('levelup-bubble-card-editor');
  }

  static getStubConfig() {
    return {
      roll_entity: 'sensor.levelup_roll',
      pitch_entity: 'sensor.levelup_pitch',
      tolerance: 1.5,
      title: 'Van Level'
    };
  }

  setConfig(config) {
    if (!config.roll_entity) throw new Error('roll_entity required');
    if (!config.pitch_entity) throw new Error('pitch_entity required');
    
    this._config = {
      tolerance: 1.5,
      title: '',
      multiplier: 5,
      show_values: true,
      size: 280,
      ...config
    };
    
    this._roll = 0;
    this._pitch = 0;
    this._hass = null;
  }

  set hass(hass) {
    this._hass = hass;
    const roll = parseFloat(hass.states[this._config.roll_entity]?.state) || 0;
    const pitch = parseFloat(hass.states[this._config.pitch_entity]?.state) || 0;
    
    if (roll !== this._roll || pitch !== this._pitch) {
      this._roll = roll;
      this._pitch = pitch;
      this._render();
    }
  }

  _render() {
    if (!this._card) return;
    
    const { tolerance, multiplier, show_values, size } = this._config;
    const roll = this._roll;
    const pitch = this._pitch;
    
    // Clamp bubble position
    const maxOffset = (size - 40) / 2 - 20;
    const bx = Math.max(-maxOffset, Math.min(maxOffset, roll * multiplier * 2.5));
    const by = Math.max(-maxOffset, Math.min(maxOffset, -pitch * multiplier * 2.5));
    
    // Level check
    const isLevel = Math.abs(roll) <= tolerance && Math.abs(pitch) <= tolerance;
    
    // Bubble position
    const bubble = this._card.querySelector('.bubble-dot');
    bubble.style.transform = `translate(${bx}px, ${by}px)`;
    bubble.classList.toggle('level', isLevel);
    
    // Values
    if (show_values) {
      this._card.querySelector('.roll-val').textContent = roll.toFixed(1);
      this._card.querySelector('.pitch-val').textContent = pitch.toFixed(1);
    }
    
    // Status
    const status = this._card.querySelector('.level-status');
    status.textContent = isLevel ? 'LEVEL' : `${Math.max(Math.abs(roll), Math.abs(pitch)).toFixed(1)}° off`;
    status.classList.toggle('level', isLevel);
    
    // Ring highlight
    this._card.querySelector('.level-ring').classList.toggle('level', isLevel);
  }

  connectedCallback() {
    const { title, size } = this._config;
    
    this.innerHTML = `
      <ha-card>
        <div class="bubble-container" style="width:${size}px;height:${size}px">
          ${title ? `<div class="bubble-title">${title}</div>` : ''}
          
          <div class="level-ring">
            <svg viewBox="0 0 200 200" class="ring-svg">
              <circle cx="100" cy="100" r="90" class="ring-bg" />
              <circle cx="100" cy="100" r="90" class="ring-fg" />
              <line x1="100" y1="10" x2="100" y2="190" class="crosshair" />
              <line x1="10" y1="100" x2="190" y2="100" class="crosshair" />
              <circle cx="100" cy="100" r="4" class="crosshair-dot" />
            </svg>
            
            <div class="bubble-dot">
              <div class="bubble-inner"></div>
            </div>
            
            <div class="axis-labels">
              <span class="axis-top">${this._hass?.states[this._config.pitch_entity]?.attributes?.unit_of_measurement || '°'}</span>
              <span class="axis-right">R</span>
            </div>
          </div>
          
          <div class="level-status">--</div>
          
          <div class="value-row">
            <div class="value-item">
              <span class="value-label">Roll</span>
              <span class="roll-val value-num">--</span>
            </div>
            <div class="value-item">
              <span class="value-label">Pitch</span>
              <span class="pitch-val value-num">--</span>
            </div>
          </div>
        </div>
      </ha-card>
    `;
    
    this._card = this.querySelector('.bubble-container');
    this._render();
  }

  getCardSize() {
    return 3;
  }
}

// Register
customElements.define('levelup-bubble-card', LevelUpBubbleCard);

// --- Styles ---
const style = document.createElement('style');
style.textContent = `
  .bubble-container {
    position: relative;
    display: flex;
    flex-direction: column;
    align-items: center;
    padding: 16px;
    box-sizing: content-box;
  }
  
  .bubble-title {
    font-family: 'Inter', -apple-system, sans-serif;
    font-size: 14px;
    font-weight: 500;
    color: var(--secondary-text-color);
    margin-bottom: 8px;
    text-transform: uppercase;
    letter-spacing: 0.5px;
  }
  
  .level-ring {
    position: relative;
    width: 100%;
    aspect-ratio: 1;
    border-radius: 50%;
    background: var(--card-background-color, #1c1c1e);
    box-shadow: inset 0 2px 8px rgba(0,0,0,0.3), 0 1px 3px rgba(255,255,255,0.05);
    transition: box-shadow 0.6s ease;
  }
  
  .level-ring.level {
    box-shadow: inset 0 2px 8px rgba(0,0,0,0.3), 0 0 24px rgba(52,199,89,0.15);
  }
  
  .ring-svg {
    position: absolute;
    inset: 0;
    width: 100%;
    height: 100%;
  }
  
  .ring-bg {
    fill: none;
    stroke: var(--divider-color, rgba(255,255,255,0.08));
    stroke-width: 2;
  }
  
  .ring-fg {
    fill: none;
    stroke: var(--primary-color, #1f6feb);
    stroke-width: 1.5;
    stroke-dasharray: 565;
    stroke-dashoffset: 565;
    transition: stroke-dashoffset 0.8s cubic-bezier(0.4,0,0.2,1), stroke 0.6s ease;
  }
  
  .level-ring.level .ring-fg {
    stroke: #34c759;
    stroke-dashoffset: 0;
  }
  
  .crosshair {
    stroke: var(--divider-color, rgba(255,255,255,0.06));
    stroke-width: 0.5;
  }
  
  .crosshair-dot {
    fill: var(--secondary-text-color);
    opacity: 0.3;
  }
  
  .bubble-dot {
    position: absolute;
    top: 50%;
    left: 50%;
    width: 32px;
    height: 32px;
    margin: -16px 0 0 -16px;
    border-radius: 50%;
    background: radial-gradient(circle at 35% 35%, rgba(255,255,255,0.4), transparent 60%),
                linear-gradient(135deg, var(--primary-color, #1f6feb), #0a84ff);
    box-shadow: 0 2px 12px rgba(31,111,235,0.4), inset 0 1px 2px rgba(255,255,255,0.3);
    transition: transform 0.15s linear, box-shadow 0.6s ease, background 0.6s ease;
    z-index: 2;
  }
  
  .bubble-dot.level {
    background: radial-gradient(circle at 35% 35%, rgba(255,255,255,0.4), transparent 60%),
                linear-gradient(135deg, #34c759, #30d158);
    box-shadow: 0 2px 16px rgba(52,199,89,0.5), inset 0 1px 2px rgba(255,255,255,0.3);
  }
  
  .bubble-inner {
    position: absolute;
    top: 6px;
    left: 6px;
    width: 10px;
    height: 6px;
    background: rgba(255,255,255,0.6);
    border-radius: 50%;
    transform: rotate(-30deg);
  }
  
  .axis-labels {
    position: absolute;
    inset: 0;
    pointer-events: none;
  }
  
  .axis-top {
    position: absolute;
    top: 8px;
    left: 50%;
    transform: translateX(-50%);
    font-size: 10px;
    color: var(--secondary-text-color);
    opacity: 0.4;
  }
  
  .axis-right {
    position: absolute;
    top: 50%;
    right: 8px;
    transform: translateY(-50%);
    font-size: 10px;
    color: var(--secondary-text-color);
    opacity: 0.4;
  }
  
  .level-status {
    margin-top: 12px;
    font-family: 'Inter', -apple-system, sans-serif;
    font-size: 13px;
    font-weight: 600;
    color: var(--secondary-text-color);
    text-transform: uppercase;
    letter-spacing: 1px;
    transition: color 0.4s ease;
  }
  
  .level-status.level {
    color: #34c759;
  }
  
  .value-row {
    display: flex;
    gap: 24px;
    margin-top: 8px;
  }
  
  .value-item {
    display: flex;
    flex-direction: column;
    align-items: center;
  }
  
  .value-label {
    font-size: 10px;
    color: var(--secondary-text-color);
    opacity: 0.5;
    text-transform: uppercase;
  }
  
  .value-num {
    font-family: 'SF Mono', 'Fira Code', monospace;
    font-size: 18px;
    font-weight: 500;
    color: var(--primary-text-color);
  }
`;

document.head.appendChild(style);
