plugins {
    id("com.android.asset-pack")
}

assetPack {
    packName.set("island")
    dynamicDelivery {
        // Downloaded together with the app on first install. No later download.
        deliveryType.set("install-time")
    }
}
