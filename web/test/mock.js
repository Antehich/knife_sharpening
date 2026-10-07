// Мок Web Bluetooth: ведёт себя как прошивка KnifeAngle.
(() => {
  const SVC = '8c900001-4d19-4014-a48c-3f940a276aeb';
  const ANGLE = '8c900002-4d19-4014-a48c-3f940a276aeb';
  const CMD = '8c900003-4d19-4014-a48c-3f940a276aeb';
  const log = (window.__btlog = []);
  function dv(angleX10, flags) {
    const b = new DataView(new ArrayBuffer(3));
    b.setInt16(0, angleX10, true); b.setUint8(2, flags); return b;
  }
  class Char extends EventTarget {
    constructor(uuid) { super(); this.uuid = uuid; this.value = null; this.notifying = false; }
    async startNotifications() { this.notifying = true; log.push('notify:' + this.uuid.slice(4, 8)); return this; }
    async readValue() { return this.value; }
    async writeValueWithResponse(d) { log.push('write:' + Array.from(new Uint8Array(d.buffer || d)).join(',')); }
    push(v) { this.value = v; if (this.notifying && this.oncharacteristicvaluechanged) this.oncharacteristicvaluechanged({ target: this }); }
  }
  const angle = new Char(ANGLE); angle.value = dv(0, 0);
  const cmd = new Char(CMD);
  const batt = new Char('battery_level'); batt.value = new DataView(new Uint8Array([87]).buffer);
  const device = new EventTarget();
  device.name = 'KnifeAngle';
  window.__failConnect = false;
  device.gatt = {
    connected: false,
    async connect() {
      log.push('connect');
      if (window.__failConnect) throw new Error('mock: unreachable');
      this.connected = true; return server;
    },
    disconnect() { this.connected = false; log.push('disconnect'); device.dispatchEvent(new Event('gattserverdisconnected')); },
  };
  const server = {
    async getPrimaryService(u) {
      if (u === SVC) return { async getCharacteristic(c) { return c === ANGLE ? angle : cmd; } };
      if (u === 'battery_service') return { async getCharacteristic() { return batt; } };
      throw new Error('no service');
    },
  };
  Object.defineProperty(navigator, 'bluetooth', { value: {
    async requestDevice(opts) { log.push('request:' + JSON.stringify(opts.filters)); return device; },
  }, configurable: true });
  window.__mock = {
    send(deg, flags) { angle.push(dv(Math.round(deg * 10), flags)); },
    drop() { device.gatt.connected = false; device.dispatchEvent(new Event('gattserverdisconnected')); },
  };
})();
