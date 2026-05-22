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
	    caps: string[];
	    roleKind: number;
	    roleName: string;
	    hardwareName: string;
	    allowedRoles: RoleOption[];
	    name: string;
	
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
	        this.caps = source["caps"];
	        this.roleKind = source["roleKind"];
	        this.roleName = source["roleName"];
	        this.hardwareName = source["hardwareName"];
	        this.allowedRoles = this.convertValues(source["allowedRoles"], RoleOption);
	        this.name = source["name"];
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
	
	export class PresetClaim {
	    domain: string;
	    slot: string;
	    port: PresetPortRef;
	
	    static createFrom(source: any = {}) {
	        return new PresetClaim(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.domain = source["domain"];
	        this.slot = source["slot"];
	        this.port = this.convertValues(source["port"], PresetPortRef);
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
	export class PresetPortRef {
	    board: string;
	    kind: number;
	    index: number;
	
	    static createFrom(source: any = {}) {
	        return new PresetPortRef(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.board = source["board"];
	        this.kind = source["kind"];
	        this.index = source["index"];
	    }
	}
	export class RoleAssign {
	    port: PresetPortRef;
	    roleKind: number;
	
	    static createFrom(source: any = {}) {
	        return new RoleAssign(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.port = this.convertValues(source["port"], PresetPortRef);
	        this.roleKind = source["roleKind"];
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
	export class Preset {
	    name: string;
	    description: string;
	    roles: RoleAssign[];
	    claims: PresetClaim[];
	
	    static createFrom(source: any = {}) {
	        return new Preset(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.name = source["name"];
	        this.description = source["description"];
	        this.roles = this.convertValues(source["roles"], RoleAssign);
	        this.claims = this.convertValues(source["claims"], PresetClaim);
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
	    presets: devicemodel.Preset[];
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
	        this.presets = this.convertValues(source["presets"], devicemodel.Preset);
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
	
	    static createFrom(source: any = {}) {
	        return new PortDescriptor(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.Index = source["Index"];
	        this.Flags = source["Flags"];
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

