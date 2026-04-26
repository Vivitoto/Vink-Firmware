#include "BlePageTurner.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

static BLEServer* pServer = nullptr;
static BLECharacteristic* pCharacteristic = nullptr;
static bool deviceConnected = false;

class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        std::string value = pCharacteristic->getValue();
        if (value.length() > 0) {
            char cmd = value[0];
            if (BlePageTurner::_instance) {
                if (cmd == 'N' || cmd == 'n') {
                    BlePageTurner::_instance->setPendingCommand(1);
                } else if (cmd == 'P' || cmd == 'p') {
                    BlePageTurner::_instance->setPendingCommand(-1);
                }
            }
        }
    }
};

class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        deviceConnected = true;
        Serial.println("[BLE] Client connected");
    }
    void onDisconnect(BLEServer* pServer) {
        deviceConnected = false;
        Serial.println("[BLE] Client disconnected");
        BLEDevice::startAdvertising();
    }
};

BlePageTurner* BlePageTurner::_instance = nullptr;

BlePageTurner::BlePageTurner() : _running(false), _pendingCommand(0) {
    _instance = this;
}

bool BlePageTurner::start() {
    if (_running) return true;
    
    Serial.println("[BLE] Starting...");
    BLEDevice::init("Vink-PaperS3");
    
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    
    BLEService *pService = pServer->createService(SERVICE_UUID);
    pCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY
    );
    pCharacteristic->setCallbacks(new MyCallbacks());
    pCharacteristic->addDescriptor(new BLE2902());
    
    pService->start();
    
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);
    pAdvertising->setMinPreferred(0x12);
    BLEDevice::startAdvertising();
    
    _running = true;
    Serial.println("[BLE] Ready, waiting for connection...");
    return true;
}

void BlePageTurner::stop() {
    if (!_running) return;
    BLEDevice::deinit(true);
    _running = false;
    Serial.println("[BLE] Stopped");
}

int BlePageTurner::checkCommand() {
    int cmd = _pendingCommand;
    _pendingCommand = 0;
    return cmd;
}

void BlePageTurner::onWriteCallback(const char* data, int len) {
    // handled in MyCallbacks
}
