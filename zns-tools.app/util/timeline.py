#! /usr/bin/python3

class Timeline:
    def __init__(self):
        self.traceEvents = []
        self.displayTimeUnit = "ns"
        self.systemTraceEvents = "SystemTraceData"
        self.otherData = dict()
        self.otherData["name"] = "zns-tools.app: ZNS SSD Application Tracing Framework"
        self.otherData["version"] = "v0.1" # TODO: assign versions here

    def addTimestamp(self, timestamp):
        self.traceEvents.append(timestamp)

    def __str__(self):
        return f"displayTimeUnit: {self.displayTimeUnit}, systemTraceEvents: {self.systemTraceEvents}, otherData: {self.otherData}, traceEvents: {self.traceEvents}"
