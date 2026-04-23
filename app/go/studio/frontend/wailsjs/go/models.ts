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
	export class SlaveInfo {
	    type: string;
	    name: string;
	    connected: boolean;
	    ready: boolean;
	
	    static createFrom(source: any = {}) {
	        return new SlaveInfo(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.type = source["type"];
	        this.name = source["name"];
	        this.connected = source["connected"];
	        this.ready = source["ready"];
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

