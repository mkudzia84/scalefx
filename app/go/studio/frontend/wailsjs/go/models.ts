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

}

