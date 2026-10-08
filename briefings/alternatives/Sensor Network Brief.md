# Sensor Network Brief

### Customer Requirement Description
The proposed solution is a LoRa Mesh-enabled environmental sensor network device designed for distributed monitoring applications. Each sensor node shall collect and transmit environmental data across a resilient LoRa Mesh network, enabling communication over large geographical areas without relying on existing communication infrastructure.

The device shall be capable of measuring:
- Air temperature
- Relative humidity
- Ambient light intensity
- Spectral light characteristics
- Wind speed
- Wind direction

To ensure data reliability and accuracy, the system shall incorporate appropriate signal processing and filtering techniques to reduce sensor noise and compensate for transient disturbances.

In addition to raw environmental measurements, the device shall perform edge-level analytics to derive higher-level metrics, including:
- **Evapotranspiration (ET)** for agricultural and environmental monitoring applications.
- **Human Thermal Comfort Index** (such as Heat Index, Wet Bulb Globe Temperature (WBGT), or Universal Thermal Climate Index (UTCI)) to assess human comfort and potential heat stress conditions.

The sensor network shall support real-time and periodic data transmission through the LoRa Mesh infrastructure, allowing data aggregation, remote monitoring, and long-range deployment in outdoor and remote environments.

### Short Product Vision Statement
Develop a LoRa Mesh-based smart environmental sensing platform capable of measuring weather and environmental conditions, computing evapotranspiration and human comfort metrics, and reliably transmitting data across a distributed sensor network for agriculture, smart city, and environmental monitoring applications.

### Suggested Engineering Requirements

| Requirement | Description |
| :--- | :--- |
| **Connectivity** | LoRa Mesh networking |
| **Temperature** | Air temperature measurement |
| **Humidity** | Relative humidity measurement |
| **Light** | Ambient light intensity measurement |
| **Spectral Analysis** | Multi-band light spectrum sensing |
| **Wind Speed** | Anemometer-based measurement |
| **Wind Direction** | Wind vane or equivalent sensing |
| **Data Processing** | Filtering, calibration, and sensor fusion |
| **Derived Metrics** | Evapotranspiration (ET) |
| **Human Comfort** | Heat Index / WBGT / UTCI |
| **Power** | Low-power operation for remote deployment |
| **Scalability** | Multi-node mesh deployment |
| **Monitoring** | Remote data collection and visualization |
