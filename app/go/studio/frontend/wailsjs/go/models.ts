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

