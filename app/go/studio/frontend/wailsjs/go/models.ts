export namespace audio {
	
	export class PreloadEntry {
	    path: string;
	    totalBytes: number;
	    loadedBytes: number;
	    status: number;
	    statusName: string;
	    format: number;
	    formatName: string;
	    owners: string[];
	
	    static createFrom(source: any = {}) {
	        return new PreloadEntry(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.path = source["path"];
	        this.totalBytes = source["totalBytes"];
	        this.loadedBytes = source["loadedBytes"];
	        this.status = source["status"];
	        this.statusName = source["statusName"];
	        this.format = source["format"];
	        this.formatName = source["formatName"];
	        this.owners = source["owners"];
	    }
	}

}

export namespace devicemodel {
	
	export class ChannelFunctionDef {
	    id: string;
	    label: string;
	    group: string;
	
	    static createFrom(source: any = {}) {
	        return new ChannelFunctionDef(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.id = source["id"];
	        this.label = source["label"];
	        this.group = source["group"];
	    }
	}
	export class ChannelMap {
	    channel: number;
	    function: string;
	
	    static createFrom(source: any = {}) {
	        return new ChannelMap(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.channel = source["channel"];
	        this.function = source["function"];
	    }
	}
	export class PortRef {
	    guid: string;
	    kind: number;
	    index: number;
	
	    static createFrom(source: any = {}) {
	        return new PortRef(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.guid = source["guid"];
	        this.kind = source["kind"];
	        this.index = source["index"];
	    }
	}
	export class Claim {
	    domain: string;
	    slot: string;
	    port: PortRef;
	
	    static createFrom(source: any = {}) {
	        return new Claim(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.domain = source["domain"];
	        this.slot = source["slot"];
	        this.port = this.convertValues(source["port"], PortRef);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class Slot {
	    key: string;
	    label: string;
	    roleKinds: number[];
	    direction: string;
	    min: number;
	    max: number;
	    optional: boolean;
	    shared: boolean;
	
	    static createFrom(source: any = {}) {
	        return new Slot(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.key = source["key"];
	        this.label = source["label"];
	        this.roleKinds = source["roleKinds"];
	        this.direction = source["direction"];
	        this.min = source["min"];
	        this.max = source["max"];
	        this.optional = source["optional"];
	        this.shared = source["shared"];
	    }
	}
	export class Domain {
	    id: string;
	    label: string;
	    cap: number;
	    slots: Slot[];
	
	    static createFrom(source: any = {}) {
	        return new Domain(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.id = source["id"];
	        this.label = source["label"];
	        this.cap = source["cap"];
	        this.slots = this.convertValues(source["slots"], Slot);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class InputPortConfig {
	    port: PortRef;
	    protocol: string;
	    channelCount: number;
	    channels: ChannelMap[];
	
	    static createFrom(source: any = {}) {
	        return new InputPortConfig(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.port = this.convertValues(source["port"], PortRef);
	        this.protocol = source["protocol"];
	        this.channelCount = source["channelCount"];
	        this.channels = this.convertValues(source["channels"], ChannelMap);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class InputProtocolDef {
	    id: string;
	    label: string;
	    roleKind: number;
	    implemented: boolean;
	    maxChannels: number;
	
	    static createFrom(source: any = {}) {
	        return new InputProtocolDef(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.id = source["id"];
	        this.label = source["label"];
	        this.roleKind = source["roleKind"];
	        this.implemented = source["implemented"];
	        this.maxChannels = source["maxChannels"];
	    }
	}
	export class Issue {
	    severity: string;
	    domain?: string;
	    slot?: string;
	    port?: PortRef;
	    message: string;
	
	    static createFrom(source: any = {}) {
	        return new Issue(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.severity = source["severity"];
	        this.domain = source["domain"];
	        this.slot = source["slot"];
	        this.port = this.convertValues(source["port"], PortRef);
	        this.message = source["message"];
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class ServoMotionProfile {
	    minUs: number;
	    maxUs: number;
	    centerUs: number;
	    reversed: boolean;
	    maxSpeedUsPerSec: number;
	    maxAccelUsPerSec2: number;
	    maxJerkUsPerSec3: number;
	
	    static createFrom(source: any = {}) {
	        return new ServoMotionProfile(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.minUs = source["minUs"];
	        this.maxUs = source["maxUs"];
	        this.centerUs = source["centerUs"];
	        this.reversed = source["reversed"];
	        this.maxSpeedUsPerSec = source["maxSpeedUsPerSec"];
	        this.maxAccelUsPerSec2 = source["maxAccelUsPerSec2"];
	        this.maxJerkUsPerSec3 = source["maxJerkUsPerSec3"];
	    }
	}
	export class RoleOption {
	    kind: number;
	    label: string;
	
	    static createFrom(source: any = {}) {
	        return new RoleOption(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.kind = source["kind"];
	        this.label = source["label"];
	    }
	}
	export class Port {
	    ref: PortRef;
	    boardName: string;
	    kindName: string;
	    direction: string;
	    flags: number;
	    voltageMv: number;
	    caps: string[];
	    roleKind: number;
	    roleName: string;
	    hardwareName: string;
	    allowedRoles: RoleOption[];
	    name: string;
	    profile?: ServoMotionProfile;
	
	    static createFrom(source: any = {}) {
	        return new Port(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.ref = this.convertValues(source["ref"], PortRef);
	        this.boardName = source["boardName"];
	        this.kindName = source["kindName"];
	        this.direction = source["direction"];
	        this.flags = source["flags"];
	        this.voltageMv = source["voltageMv"];
	        this.caps = source["caps"];
	        this.roleKind = source["roleKind"];
	        this.roleName = source["roleName"];
	        this.hardwareName = source["hardwareName"];
	        this.allowedRoles = this.convertValues(source["allowedRoles"], RoleOption);
	        this.name = source["name"];
	        this.profile = this.convertValues(source["profile"], ServoMotionProfile);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	
	
	

}

export namespace expanders {
	
	export class BatteryInfo {
	    valid: boolean;
	    present: boolean;
	    voltage_mV: number;
	    cellCount: number;
	    pct: number;
	    flags: number;
	
	    static createFrom(source: any = {}) {
	        return new BatteryInfo(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.valid = source["valid"];
	        this.present = source["present"];
	        this.voltage_mV = source["voltage_mV"];
	        this.cellCount = source["cellCount"];
	        this.pct = source["pct"];
	        this.flags = source["flags"];
	    }
	}
	export class ExpanderEntry {
	    kind: number;
	    kindName: string;
	    usbAddr: number;
	    vid: number;
	    pid: number;
	    identified: boolean;
	    collision: boolean;
	    guid?: string;
	    deviceName?: string;
	    firmwareVersion?: string;
	    capabilities?: number;
	    buildNumber?: number;
	    battery?: BatteryInfo;
	
	    static createFrom(source: any = {}) {
	        return new ExpanderEntry(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.kind = source["kind"];
	        this.kindName = source["kindName"];
	        this.usbAddr = source["usbAddr"];
	        this.vid = source["vid"];
	        this.pid = source["pid"];
	        this.identified = source["identified"];
	        this.collision = source["collision"];
	        this.guid = source["guid"];
	        this.deviceName = source["deviceName"];
	        this.firmwareVersion = source["firmwareVersion"];
	        this.capabilities = source["capabilities"];
	        this.buildNumber = source["buildNumber"];
	        this.battery = this.convertValues(source["battery"], BatteryInfo);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class HubInfo {
	    guid: string;
	    deviceName: string;
	    firmwareVersion: string;
	    platform: string;
	    cpuFreqMHz: number;
	    freeRamBytes: number;
	    buildNumber: number;
	    capabilities: number;
	
	    static createFrom(source: any = {}) {
	        return new HubInfo(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.guid = source["guid"];
	        this.deviceName = source["deviceName"];
	        this.firmwareVersion = source["firmwareVersion"];
	        this.platform = source["platform"];
	        this.cpuFreqMHz = source["cpuFreqMHz"];
	        this.freeRamBytes = source["freeRamBytes"];
	        this.buildNumber = source["buildNumber"];
	        this.capabilities = source["capabilities"];
	    }
	}
	export class SystemInfo {
	    hub: HubInfo;
	    expanders: ExpanderEntry[];
	
	    static createFrom(source: any = {}) {
	        return new SystemInfo(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.hub = this.convertValues(source["hub"], HubInfo);
	        this.expanders = this.convertValues(source["expanders"], ExpanderEntry);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}

}

export namespace main {
	
	export class ProgramLandingBindingDTO {
	    id: number;
	    state: string;
	
	    static createFrom(source: any = {}) {
	        return new ProgramLandingBindingDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.id = source["id"];
	        this.state = source["state"];
	    }
	}
	export class PortRefDTO {
	    board: string;
	    guid: string;
	    kind: string;
	    idx: number;
	
	    static createFrom(source: any = {}) {
	        return new PortRefDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.board = source["board"];
	        this.guid = source["guid"];
	        this.kind = source["kind"];
	        this.idx = source["idx"];
	    }
	}
	export class ProgramChannelDTO {
	    name: string;
	    port: PortRefDTO;
	    brightnessPct: number;
	    loop: boolean;
	    events: ProgramEventDTO[];
	
	    static createFrom(source: any = {}) {
	        return new ProgramChannelDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.name = source["name"];
	        this.port = this.convertValues(source["port"], PortRefDTO);
	        this.brightnessPct = source["brightnessPct"];
	        this.loop = source["loop"];
	        this.events = this.convertValues(source["events"], ProgramEventDTO);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class ProgramEventDTO {
	    kind: string;
	    durationMs: number;
	    cycleMs: number;
	    brightnessPct: number;
	    minPct: number;
	    maxPct: number;
	    flashPct: number;
	
	    static createFrom(source: any = {}) {
	        return new ProgramEventDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.kind = source["kind"];
	        this.durationMs = source["durationMs"];
	        this.cycleMs = source["cycleMs"];
	        this.brightnessPct = source["brightnessPct"];
	        this.minPct = source["minPct"];
	        this.maxPct = source["maxPct"];
	        this.flashPct = source["flashPct"];
	    }
	}
	export class TrackDTO {
	    channel: string;
	    brightnessPct: number;
	    loop: boolean;
	    events: ProgramEventDTO[];
	
	    static createFrom(source: any = {}) {
	        return new TrackDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.channel = source["channel"];
	        this.brightnessPct = source["brightnessPct"];
	        this.loop = source["loop"];
	        this.events = this.convertValues(source["events"], ProgramEventDTO);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class ProgramDTO {
	    schemaVersion: number;
	    tracks: TrackDTO[];
	    channels: ProgramChannelDTO[];
	    landingBindings: ProgramLandingBindingDTO[];
	
	    static createFrom(source: any = {}) {
	        return new ProgramDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.schemaVersion = source["schemaVersion"];
	        this.tracks = this.convertValues(source["tracks"], TrackDTO);
	        this.channels = this.convertValues(source["channels"], ProgramChannelDTO);
	        this.landingBindings = this.convertValues(source["landingBindings"], ProgramLandingBindingDTO);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class ActiveProgramDTO {
	    name: string;
	    program: ProgramDTO;
	
	    static createFrom(source: any = {}) {
	        return new ActiveProgramDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.name = source["name"];
	        this.program = this.convertValues(source["program"], ProgramDTO);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class AudioPreloadStatus {
	    residentBytes: number;
	    budgetBytes: number;
	    ready: number;
	    loading: number;
	    failed: number;
	    pinned: number;
	    entries: audio.PreloadEntry[];
	
	    static createFrom(source: any = {}) {
	        return new AudioPreloadStatus(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.residentBytes = source["residentBytes"];
	        this.budgetBytes = source["budgetBytes"];
	        this.ready = source["ready"];
	        this.loading = source["loading"];
	        this.failed = source["failed"];
	        this.pinned = source["pinned"];
	        this.entries = this.convertValues(source["entries"], audio.PreloadEntry);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class ConnectionInfo {
	    connected: boolean;
	    initialized: boolean;
	    port: string;
	    controllerType: string;
	    controllerName: string;
	    firmwareVersion: string;
	    build: number;
	    platform: string;
	    cpuMHz: number;
	    freeRAM: number;
	    capabilities: number;
	
	    static createFrom(source: any = {}) {
	        return new ConnectionInfo(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.connected = source["connected"];
	        this.initialized = source["initialized"];
	        this.port = source["port"];
	        this.controllerType = source["controllerType"];
	        this.controllerName = source["controllerName"];
	        this.firmwareVersion = source["firmwareVersion"];
	        this.build = source["build"];
	        this.platform = source["platform"];
	        this.cpuMHz = source["cpuMHz"];
	        this.freeRAM = source["freeRAM"];
	        this.capabilities = source["capabilities"];
	    }
	}
	export class DeviceInfo {
	    capabilities: number;
	    capabilityNames: string[];
	    hasTopology: boolean;
	    system?: expanders.SystemInfo;
	    ports?: topology.BoardPorts[];
	
	    static createFrom(source: any = {}) {
	        return new DeviceInfo(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.capabilities = source["capabilities"];
	        this.capabilityNames = source["capabilityNames"];
	        this.hasTopology = source["hasTopology"];
	        this.system = this.convertValues(source["system"], expanders.SystemInfo);
	        this.ports = this.convertValues(source["ports"], topology.BoardPorts);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class RoleKindInfo {
	    kind: number;
	    name: string;
	    label: string;
	
	    static createFrom(source: any = {}) {
	        return new RoleKindInfo(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.kind = source["kind"];
	        this.name = source["name"];
	        this.label = source["label"];
	    }
	}
	export class DeviceModelSnapshot {
	    ports: devicemodel.Port[];
	    claims: devicemodel.Claim[];
	    domains: devicemodel.Domain[];
	    issues: devicemodel.Issue[];
	    roleCatalog: RoleKindInfo[];
	    inputs: devicemodel.InputPortConfig[];
	    channelFunctions: devicemodel.ChannelFunctionDef[];
	    inputProtocols: devicemodel.InputProtocolDef[];
	
	    static createFrom(source: any = {}) {
	        return new DeviceModelSnapshot(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.ports = this.convertValues(source["ports"], devicemodel.Port);
	        this.claims = this.convertValues(source["claims"], devicemodel.Claim);
	        this.domains = this.convertValues(source["domains"], devicemodel.Domain);
	        this.issues = this.convertValues(source["issues"], devicemodel.Issue);
	        this.roleCatalog = this.convertValues(source["roleCatalog"], RoleKindInfo);
	        this.inputs = this.convertValues(source["inputs"], devicemodel.InputPortConfig);
	        this.channelFunctions = this.convertValues(source["channelFunctions"], devicemodel.ChannelFunctionDef);
	        this.inputProtocols = this.convertValues(source["inputProtocols"], devicemodel.InputProtocolDef);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class DeviceStatus {
	    uptimeMs: number;
	    freeRamBytes: number;
	    freeDramBytes: number;
	    freePsramBytes: number;
	    hasMemExtension: boolean;
	    keepaliveCount: number;
	    boardStateName: string;
	    boardStateDisplay: string;
	
	    static createFrom(source: any = {}) {
	        return new DeviceStatus(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.uptimeMs = source["uptimeMs"];
	        this.freeRamBytes = source["freeRamBytes"];
	        this.freeDramBytes = source["freeDramBytes"];
	        this.freePsramBytes = source["freePsramBytes"];
	        this.hasMemExtension = source["hasMemExtension"];
	        this.keepaliveCount = source["keepaliveCount"];
	        this.boardStateName = source["boardStateName"];
	        this.boardStateDisplay = source["boardStateDisplay"];
	    }
	}
	export class DiagEvent {
	    time: string;
	    level: string;
	    tag: string;
	    message: string;
	    fields?: Record<string, any>;
	
	    static createFrom(source: any = {}) {
	        return new DiagEvent(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.time = source["time"];
	        this.level = source["level"];
	        this.tag = source["tag"];
	        this.message = source["message"];
	        this.fields = source["fields"];
	    }
	}
	export class EngineTransitions {
	    startingOffsetMs: number;
	    stoppingOffsetMs: number;
	    startFadeInMs: number;
	    stopFadeOutMs: number;
	
	    static createFrom(source: any = {}) {
	        return new EngineTransitions(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.startingOffsetMs = source["startingOffsetMs"];
	        this.stoppingOffsetMs = source["stoppingOffsetMs"];
	        this.startFadeInMs = source["startFadeInMs"];
	        this.stopFadeOutMs = source["stopFadeOutMs"];
	    }
	}
	export class EngineSounds {
	    starting: string;
	    running: string;
	    stopping: string;
	    transitions: EngineTransitions;
	
	    static createFrom(source: any = {}) {
	        return new EngineSounds(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.starting = source["starting"];
	        this.running = source["running"];
	        this.stopping = source["stopping"];
	        this.transitions = this.convertValues(source["transitions"], EngineTransitions);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class EngineToggle {
	    input: string;
	    thresholdUs: number;
	    hysteresisUs: number;
	    failsafe: string;
	
	    static createFrom(source: any = {}) {
	        return new EngineToggle(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.input = source["input"];
	        this.thresholdUs = source["thresholdUs"];
	        this.hysteresisUs = source["hysteresisUs"];
	        this.failsafe = source["failsafe"];
	    }
	}
	export class EngineConfig {
	    schemaVersion: number;
	    enabled: boolean;
	    type: string;
	    output: string;
	    toggle: EngineToggle;
	    sounds: EngineSounds;
	
	    static createFrom(source: any = {}) {
	        return new EngineConfig(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.schemaVersion = source["schemaVersion"];
	        this.enabled = source["enabled"];
	        this.type = source["type"];
	        this.output = source["output"];
	        this.toggle = this.convertValues(source["toggle"], EngineToggle);
	        this.sounds = this.convertValues(source["sounds"], EngineSounds);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	
	export class EngineStatusDTO {
	    state: number;
	    stateName: string;
	    engaged: boolean;
	
	    static createFrom(source: any = {}) {
	        return new EngineStatusDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.state = source["state"];
	        this.stateName = source["stateName"];
	        this.engaged = source["engaged"];
	    }
	}
	
	
	export class FanDTO {
	    port: PortRefDTO;
	    elementMv: number;
	    mode: string;
	    pulseDurationMs: number;
	
	    static createFrom(source: any = {}) {
	        return new FanDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.port = this.convertValues(source["port"], PortRefDTO);
	        this.elementMv = source["elementMv"];
	        this.mode = source["mode"];
	        this.pulseDurationMs = source["pulseDurationMs"];
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class FileCheck {
	    path: string;
	    exists: boolean;
	    size: number;
	    err?: string;
	
	    static createFrom(source: any = {}) {
	        return new FileCheck(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.path = source["path"];
	        this.exists = source["exists"];
	        this.size = source["size"];
	        this.err = source["err"];
	    }
	}
	export class FirmwareTarget {
	    name: string;
	    platform: string;
	    subDir: string;
	
	    static createFrom(source: any = {}) {
	        return new FirmwareTarget(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.name = source["name"];
	        this.platform = source["platform"];
	        this.subDir = source["subDir"];
	    }
	}
	export class FirmwareVersionInfo {
	    version?: string;
	    build?: number;
	    error?: string;
	
	    static createFrom(source: any = {}) {
	        return new FirmwareVersionInfo(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.version = source["version"];
	        this.build = source["build"];
	        this.error = source["error"];
	    }
	}
	export class FsEntry {
	    name: string;
	    isDir: boolean;
	    size: number;
	
	    static createFrom(source: any = {}) {
	        return new FsEntry(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.name = source["name"];
	        this.isDir = source["isDir"];
	        this.size = source["size"];
	    }
	}
	export class FsStorageStatus {
	    flashAvailable: boolean;
	    flashTotal: number;
	    flashUsed: number;
	    flashFree: number;
	    sdAvailable: boolean;
	    sdCardMB: number;
	    sdTotalMB: number;
	    sdUsedMB: number;
	    sdFreeMB: number;
	    sdCardType: string;
	    sdBusMode: string;
	
	    static createFrom(source: any = {}) {
	        return new FsStorageStatus(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.flashAvailable = source["flashAvailable"];
	        this.flashTotal = source["flashTotal"];
	        this.flashUsed = source["flashUsed"];
	        this.flashFree = source["flashFree"];
	        this.sdAvailable = source["sdAvailable"];
	        this.sdCardMB = source["sdCardMB"];
	        this.sdTotalMB = source["sdTotalMB"];
	        this.sdUsedMB = source["sdUsedMB"];
	        this.sdFreeMB = source["sdFreeMB"];
	        this.sdCardType = source["sdCardType"];
	        this.sdBusMode = source["sdBusMode"];
	    }
	}
	export class GunAxisDTO {
	    enabled: boolean;
	    servoPort: PortRefDTO;
	    input: string;
	    neutralUs: number;
	
	    static createFrom(source: any = {}) {
	        return new GunAxisDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.enabled = source["enabled"];
	        this.servoPort = this.convertValues(source["servoPort"], PortRefDTO);
	        this.input = source["input"];
	        this.neutralUs = source["neutralUs"];
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class HeaterActivationDTO {
	    input: string;
	    thresholdUs: number;
	    hysteresisUs: number;
	
	    static createFrom(source: any = {}) {
	        return new HeaterActivationDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.input = source["input"];
	        this.thresholdUs = source["thresholdUs"];
	        this.hysteresisUs = source["hysteresisUs"];
	    }
	}
	export class HeaterDTO {
	    port: PortRefDTO;
	    elementMv: number;
	    mode: string;
	    cycleOnMs: number;
	    cycleOffMs: number;
	    activation: HeaterActivationDTO;
	
	    static createFrom(source: any = {}) {
	        return new HeaterDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.port = this.convertValues(source["port"], PortRefDTO);
	        this.elementMv = source["elementMv"];
	        this.mode = source["mode"];
	        this.cycleOnMs = source["cycleOnMs"];
	        this.cycleOffMs = source["cycleOffMs"];
	        this.activation = this.convertValues(source["activation"], HeaterActivationDTO);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class SmokeConfigDTO {
	    heater: HeaterDTO;
	    fan: FanDTO;
	
	    static createFrom(source: any = {}) {
	        return new SmokeConfigDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.heater = this.convertValues(source["heater"], HeaterDTO);
	        this.fan = this.convertValues(source["fan"], FanDTO);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class RecoilConfigDTO {
	    enabled: boolean;
	    jerkUs: number;
	    holdMs: number;
	
	    static createFrom(source: any = {}) {
	        return new RecoilConfigDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.enabled = source["enabled"];
	        this.jerkUs = source["jerkUs"];
	        this.holdMs = source["holdMs"];
	    }
	}
	export class MuzzleFlashDTO {
	    port: PortRefDTO;
	    durationMs: number;
	    brightness: number;
	
	    static createFrom(source: any = {}) {
	        return new MuzzleFlashDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.port = this.convertValues(source["port"], PortRefDTO);
	        this.durationMs = source["durationMs"];
	        this.brightness = source["brightness"];
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class RofItemDTO {
	    name: string;
	    bandLoUs: number;
	    bandHiUs: number;
	    rpm: number;
	    soundPath: string;
	    outputMask: number;
	
	    static createFrom(source: any = {}) {
	        return new RofItemDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.name = source["name"];
	        this.bandLoUs = source["bandLoUs"];
	        this.bandHiUs = source["bandHiUs"];
	        this.rpm = source["rpm"];
	        this.soundPath = source["soundPath"];
	        this.outputMask = source["outputMask"];
	    }
	}
	export class RofConfigDTO {
	    input: string;
	    items: RofItemDTO[];
	
	    static createFrom(source: any = {}) {
	        return new RofConfigDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.input = source["input"];
	        this.items = this.convertValues(source["items"], RofItemDTO);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class TriggerConfigDTO {
	    input: string;
	    thresholdUs: number;
	    hysteresisUs: number;
	
	    static createFrom(source: any = {}) {
	        return new TriggerConfigDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.input = source["input"];
	        this.thresholdUs = source["thresholdUs"];
	        this.hysteresisUs = source["hysteresisUs"];
	    }
	}
	export class GunDTO {
	    id: number;
	    name: string;
	    trigger: TriggerConfigDTO;
	    rof: RofConfigDTO;
	    muzzleFlash: MuzzleFlashDTO;
	    recoil: RecoilConfigDTO;
	    smoke: SmokeConfigDTO;
	    yaw: GunAxisDTO;
	    pitch: GunAxisDTO;
	
	    static createFrom(source: any = {}) {
	        return new GunDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.id = source["id"];
	        this.name = source["name"];
	        this.trigger = this.convertValues(source["trigger"], TriggerConfigDTO);
	        this.rof = this.convertValues(source["rof"], RofConfigDTO);
	        this.muzzleFlash = this.convertValues(source["muzzleFlash"], MuzzleFlashDTO);
	        this.recoil = this.convertValues(source["recoil"], RecoilConfigDTO);
	        this.smoke = this.convertValues(source["smoke"], SmokeConfigDTO);
	        this.yaw = this.convertValues(source["yaw"], GunAxisDTO);
	        this.pitch = this.convertValues(source["pitch"], GunAxisDTO);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class GunFxConfig {
	    schemaVersion: number;
	    enabled: boolean;
	    guns: GunDTO[];
	
	    static createFrom(source: any = {}) {
	        return new GunFxConfig(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.schemaVersion = source["schemaVersion"];
	        this.enabled = source["enabled"];
	        this.guns = this.convertValues(source["guns"], GunDTO);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class GunManualStateDTO {
	    flags: number;
	    yawUs: number;
	    pitchUs: number;
	    rofIndex: number;
	    fireHold: number;
	    smokeArm: number;
	    smokeFanBurst: number;
	
	    static createFrom(source: any = {}) {
	        return new GunManualStateDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.flags = source["flags"];
	        this.yawUs = source["yawUs"];
	        this.pitchUs = source["pitchUs"];
	        this.rofIndex = source["rofIndex"];
	        this.fireHold = source["fireHold"];
	        this.smokeArm = source["smokeArm"];
	        this.smokeFanBurst = source["smokeFanBurst"];
	    }
	}
	export class GunStatusDTO {
	    id: number;
	    firing: boolean;
	    smokeArmed: boolean;
	
	    static createFrom(source: any = {}) {
	        return new GunStatusDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.id = source["id"];
	        this.firing = source["firing"];
	        this.smokeArmed = source["smokeArmed"];
	    }
	}
	
	
	export class HeaterElementDTO {
	    elementMv: number;
	    scaling: number;
	    drivePct: number;
	    hystCx10: number;
	    portRailMv: number;
	
	    static createFrom(source: any = {}) {
	        return new HeaterElementDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.elementMv = source["elementMv"];
	        this.scaling = source["scaling"];
	        this.drivePct = source["drivePct"];
	        this.hystCx10 = source["hystCx10"];
	        this.portRailMv = source["portRailMv"];
	    }
	}
	export class LandingActivationSource {
	    mode: string;
	    input: string;
	    thresholdUs: number;
	    hysteresisUs: number;
	    program: string;
	    whenProgram: string;
	
	    static createFrom(source: any = {}) {
	        return new LandingActivationSource(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.mode = source["mode"];
	        this.input = source["input"];
	        this.thresholdUs = source["thresholdUs"];
	        this.hysteresisUs = source["hysteresisUs"];
	        this.program = source["program"];
	        this.whenProgram = source["whenProgram"];
	    }
	}
	export class LandingLedDTO {
	    port: PortRefDTO;
	    brightnessPct: number;
	
	    static createFrom(source: any = {}) {
	        return new LandingLedDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.port = this.convertValues(source["port"], PortRefDTO);
	        this.brightnessPct = source["brightnessPct"];
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class LandingServoDTO {
	    port: PortRefDTO;
	
	    static createFrom(source: any = {}) {
	        return new LandingServoDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.port = this.convertValues(source["port"], PortRefDTO);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class LandingLightDTO {
	    id: number;
	    name: string;
	    owner: string;
	    servos: LandingServoDTO[];
	    openUs: number;
	    closeUs: number;
	    leds: LandingLedDTO[];
	    activation: LandingActivationSource;
	
	    static createFrom(source: any = {}) {
	        return new LandingLightDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.id = source["id"];
	        this.name = source["name"];
	        this.owner = source["owner"];
	        this.servos = this.convertValues(source["servos"], LandingServoDTO);
	        this.openUs = source["openUs"];
	        this.closeUs = source["closeUs"];
	        this.leds = this.convertValues(source["leds"], LandingLedDTO);
	        this.activation = this.convertValues(source["activation"], LandingActivationSource);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class LandingConfigDTO {
	    schemaVersion: number;
	    lights: LandingLightDTO[];
	
	    static createFrom(source: any = {}) {
	        return new LandingConfigDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.schemaVersion = source["schemaVersion"];
	        this.lights = this.convertValues(source["lights"], LandingLightDTO);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	
	
	
	export class LandingStatusDTO {
	    id: number;
	    phase: number;
	    phaseName: string;
	    owner: number;
	
	    static createFrom(source: any = {}) {
	        return new LandingStatusDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.id = source["id"];
	        this.phase = source["phase"];
	        this.phaseName = source["phaseName"];
	        this.owner = source["owner"];
	    }
	}
	export class LightEventInput {
	    kind: string;
	    durationMs: number;
	    cycleMs: number;
	    brightnessPct: number;
	    minPct: number;
	    maxPct: number;
	    flashPct: number;
	
	    static createFrom(source: any = {}) {
	        return new LightEventInput(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.kind = source["kind"];
	        this.durationMs = source["durationMs"];
	        this.cycleMs = source["cycleMs"];
	        this.brightnessPct = source["brightnessPct"];
	        this.minPct = source["minPct"];
	        this.maxPct = source["maxPct"];
	        this.flashPct = source["flashPct"];
	    }
	}
	export class LightFxChannelDTO {
	    name: string;
	    port?: PortRefDTO;
	    defaultBrightnessPct: number;
	
	    static createFrom(source: any = {}) {
	        return new LightFxChannelDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.name = source["name"];
	        this.port = this.convertValues(source["port"], PortRefDTO);
	        this.defaultBrightnessPct = source["defaultBrightnessPct"];
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class ProgramSelectorRangeDTO {
	    fromUs: number;
	    toUs: number;
	    program: string;
	
	    static createFrom(source: any = {}) {
	        return new ProgramSelectorRangeDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.fromUs = source["fromUs"];
	        this.toUs = source["toUs"];
	        this.program = source["program"];
	    }
	}
	export class ProgramSelectorDTO {
	    enabled: boolean;
	    input: string;
	    hysteresisUs: number;
	    ranges: ProgramSelectorRangeDTO[];
	
	    static createFrom(source: any = {}) {
	        return new ProgramSelectorDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.enabled = source["enabled"];
	        this.input = source["input"];
	        this.hysteresisUs = source["hysteresisUs"];
	        this.ranges = this.convertValues(source["ranges"], ProgramSelectorRangeDTO);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class LightFxConfigDTO {
	    schemaVersion: number;
	    enabled: boolean;
	    masterBrightnessPct: number;
	    channels: LightFxChannelDTO[];
	    programs: string[];
	    programSelector: ProgramSelectorDTO;
	
	    static createFrom(source: any = {}) {
	        return new LightFxConfigDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.schemaVersion = source["schemaVersion"];
	        this.enabled = source["enabled"];
	        this.masterBrightnessPct = source["masterBrightnessPct"];
	        this.channels = this.convertValues(source["channels"], LightFxChannelDTO);
	        this.programs = source["programs"];
	        this.programSelector = this.convertValues(source["programSelector"], ProgramSelectorDTO);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	export class LightFxStatusDTO {
	    activeIdx: number;
	    activeName: string;
	    masterBrightnessPct: number;
	
	    static createFrom(source: any = {}) {
	        return new LightFxStatusDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.activeIdx = source["activeIdx"];
	        this.activeName = source["activeName"];
	        this.masterBrightnessPct = source["masterBrightnessPct"];
	    }
	}
	export class MotorElementDTO {
	    elementMv: number;
	    scaling: number;
	    portRailMv: number;
	
	    static createFrom(source: any = {}) {
	        return new MotorElementDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.elementMv = source["elementMv"];
	        this.scaling = source["scaling"];
	        this.portRailMv = source["portRailMv"];
	    }
	}
	
	export class OpenedFile {
	    path: string;
	    content: string;
	
	    static createFrom(source: any = {}) {
	        return new OpenedFile(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.path = source["path"];
	        this.content = source["content"];
	    }
	}
	export class PortInfo {
	    name: string;
	    description: string;
	
	    static createFrom(source: any = {}) {
	        return new PortInfo(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.name = source["name"];
	        this.description = source["description"];
	    }
	}
	
	export class PresetLibraryEntry {
	    name: string;
	    source: string;
	    category: string;
	    caption: string;
	    note: string;
	    program: ProgramDTO;
	
	    static createFrom(source: any = {}) {
	        return new PresetLibraryEntry(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.name = source["name"];
	        this.source = source["source"];
	        this.category = source["category"];
	        this.caption = source["caption"];
	        this.note = source["note"];
	        this.program = this.convertValues(source["program"], ProgramDTO);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}
	
	
	
	export class ProgramFileInfo {
	    path: string;
	    name: string;
	
	    static createFrom(source: any = {}) {
	        return new ProgramFileInfo(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.path = source["path"];
	        this.name = source["name"];
	    }
	}
	
	
	
	
	export class ReleaseInfo {
	    controller: string;
	    version: string;
	    tag: string;
	    name: string;
	    body: string;
	    prerelease: boolean;
	    published: string;
	    assetName: string;
	    assetSize: number;
	
	    static createFrom(source: any = {}) {
	        return new ReleaseInfo(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.controller = source["controller"];
	        this.version = source["version"];
	        this.tag = source["tag"];
	        this.name = source["name"];
	        this.body = source["body"];
	        this.prerelease = source["prerelease"];
	        this.published = source["published"];
	        this.assetName = source["assetName"];
	        this.assetSize = source["assetSize"];
	    }
	}
	
	
	
	export class ServoMotionProfileDTO {
	    minUs: number;
	    maxUs: number;
	    centerUs: number;
	    reversed: boolean;
	    maxSpeedUsPerSec: number;
	    maxAccelUsPerSec2: number;
	    maxJerkUsPerSec3: number;
	
	    static createFrom(source: any = {}) {
	        return new ServoMotionProfileDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.minUs = source["minUs"];
	        this.maxUs = source["maxUs"];
	        this.centerUs = source["centerUs"];
	        this.reversed = source["reversed"];
	        this.maxSpeedUsPerSec = source["maxSpeedUsPerSec"];
	        this.maxAccelUsPerSec2 = source["maxAccelUsPerSec2"];
	        this.maxJerkUsPerSec3 = source["maxJerkUsPerSec3"];
	    }
	}
	export class ServoProfileDTO {
	    minUs: number;
	    maxUs: number;
	    maxSpeedUsPerSec: number;
	    reversed: boolean;
	    centerUs: number;
	    maxAccelUsPerSec2: number;
	    maxJerkUsPerSec3: number;
	
	    static createFrom(source: any = {}) {
	        return new ServoProfileDTO(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.minUs = source["minUs"];
	        this.maxUs = source["maxUs"];
	        this.maxSpeedUsPerSec = source["maxSpeedUsPerSec"];
	        this.reversed = source["reversed"];
	        this.centerUs = source["centerUs"];
	        this.maxAccelUsPerSec2 = source["maxAccelUsPerSec2"];
	        this.maxJerkUsPerSec3 = source["maxJerkUsPerSec3"];
	    }
	}
	
	export class ToolsStatus {
	    esptoolInstalled: boolean;
	    esptoolPath: string;
	    esptoolSource: string;
	
	    static createFrom(source: any = {}) {
	        return new ToolsStatus(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.esptoolInstalled = source["esptoolInstalled"];
	        this.esptoolPath = source["esptoolPath"];
	        this.esptoolSource = source["esptoolSource"];
	    }
	}
	

}

export namespace ports {
	
	export class PortDescriptor {
	    Index: number;
	    Flags: number;
	    VoltageMv: number;
	
	    static createFrom(source: any = {}) {
	        return new PortDescriptor(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.Index = source["Index"];
	        this.Flags = source["Flags"];
	        this.VoltageMv = source["VoltageMv"];
	    }
	}
	export class PortList {
	    Servos: PortDescriptor[];
	    Pwms: PortDescriptor[];
	    HBridges: PortDescriptor[];
	    Inputs: PortDescriptor[];
	
	    static createFrom(source: any = {}) {
	        return new PortList(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.Servos = this.convertValues(source["Servos"], PortDescriptor);
	        this.Pwms = this.convertValues(source["Pwms"], PortDescriptor);
	        this.HBridges = this.convertValues(source["HBridges"], PortDescriptor);
	        this.Inputs = this.convertValues(source["Inputs"], PortDescriptor);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}

}

export namespace storage {
	
	export class UploadDiag {
	    bytesRecv: number;
	    expectedSize: number;
	    segIndex: number;
	    segCount: number;
	    fillPct: number;
	    sdWriteCount: number;
	    sdBytesWritten: number;
	    sdMaxLatMs: number;
	    sdTotalStallMs: number;
	    maxLoopGapMs: number;
	    uploadActive: boolean;
	    streamActive: boolean;
	    reason: number;
	
	    static createFrom(source: any = {}) {
	        return new UploadDiag(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.bytesRecv = source["bytesRecv"];
	        this.expectedSize = source["expectedSize"];
	        this.segIndex = source["segIndex"];
	        this.segCount = source["segCount"];
	        this.fillPct = source["fillPct"];
	        this.sdWriteCount = source["sdWriteCount"];
	        this.sdBytesWritten = source["sdBytesWritten"];
	        this.sdMaxLatMs = source["sdMaxLatMs"];
	        this.sdTotalStallMs = source["sdTotalStallMs"];
	        this.maxLoopGapMs = source["maxLoopGapMs"];
	        this.uploadActive = source["uploadActive"];
	        this.streamActive = source["streamActive"];
	        this.reason = source["reason"];
	    }
	}

}

export namespace topology {
	
	export class BoardPorts {
	    guid: string;
	    deviceName: string;
	    ports: ports.PortList;
	
	    static createFrom(source: any = {}) {
	        return new BoardPorts(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.guid = source["guid"];
	        this.deviceName = source["deviceName"];
	        this.ports = this.convertValues(source["ports"], ports.PortList);
	    }
	
		convertValues(a: any, classs: any, asMap: boolean = false): any {
		    if (!a) {
		        return a;
		    }
		    if (a.slice && a.map) {
		        return (a as any[]).map(elem => this.convertValues(elem, classs));
		    } else if ("object" === typeof a) {
		        if (asMap) {
		            for (const key of Object.keys(a)) {
		                a[key] = new classs(a[key]);
		            }
		            return a;
		        }
		        return new classs(a);
		    }
		    return a;
		}
	}

}

