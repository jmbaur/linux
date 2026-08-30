#include <linux/delay.h>
#include <linux/hw_random.h>
#include <linux/i2c.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>

#define RNG90_RESET 0x0 /* resets address counter */
#define RNG90_SLEEP 0x1
#define RNG90_COMMAND 0x3
#define RNG90_OPCODE_RANDOM 0x16
#define RNG90_RANDOM_MAX_WAIT_MS 72
#define RNG90_SELFTEST_MAX_WAIT_MS 50

struct rng90_trng {
	struct hwrng rng;
	struct device *dev;
	struct i2c_client *client;
};

static uint16_t rng90_crc16(const uint8_t *data, size_t len)
{
	uint16_t crc = 0x0000;
	size_t i;

	for (i = 0; i < len; i++) {
		uint8_t byte = data[i];
		uint8_t shift;

		for (shift = 0x01; shift != 0; shift <<= 1) {
			bool dbit = (byte & shift) != 0;
			bool cbit = (crc & 0x8000) != 0;

			crc <<= 1;
			if (dbit != cbit) {
				crc ^= 0x8005;
			}
		}
	}

	return crc;
}

static int rng90_trng_init(struct hwrng *rng)
{
	struct rng90_trng *trng = container_of(rng, struct rng90_trng, rng);
	uint8_t pkt[1] = { RNG90_RESET };
	int ret;

	ret = i2c_master_send(trng->client, pkt, sizeof(pkt));
	if (ret < 0) {
		dev_err(trng->dev, "RNG reset failed: %d\n", ret);
		return ret;
	}

	dev_info(trng->dev, "RNG initialized\n");

	return 0;
}

static int rng90_trng_read(struct hwrng *rng, void *buf, size_t max, bool wait)
{
	struct rng90_trng *trng = container_of(rng, struct rng90_trng, rng);
	uint8_t response[35];
	uint8_t pkt[28];
	uint16_t crc;
	int ret;

	memset(pkt, 0, sizeof(pkt));

	pkt[0] = RNG90_COMMAND;
	pkt[1] = sizeof(pkt) - 1;
	pkt[2] = RNG90_OPCODE_RANDOM;

	crc = rng90_crc16(&pkt[1], 25);
	pkt[26] = (uint8_t)(crc & 0xff);
	pkt[27] = (uint8_t)(crc >> 8);

	ret = i2c_master_send(trng->client, pkt, sizeof(pkt));
	if (ret < 0) {
		dev_err(trng->dev, "Random command send failed: %d\n", ret);
		return ret;
	}

	msleep(RNG90_RANDOM_MAX_WAIT_MS);

	ret = i2c_master_recv(trng->client, response, sizeof(response));
	if (ret < 0) {
		dev_err(trng->dev, "Random command recv failed: %d\n", ret);
		return ret;
	}

	// response[0]: count
	// <32 bytes of random data>
	// response[33]: crc lo
	// response[34]: crc hi
	if (response[0] != sizeof(response)) {
		dev_err(trng->dev, "Got invalid RNG response length: %d\n",
			response[0]);
		return -EIO;
	}

	memcpy(buf, &response[1], min(max, 32));

	return min(max, 32);
}

static int rng90_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct rng90_trng *trng;
	int ret;

	trng = devm_kzalloc(dev, sizeof(*trng), GFP_KERNEL);
	if (!trng)
		return -ENOMEM;

	trng->dev = dev;
	trng->client = client;
	trng->rng.name = client->name;
	trng->rng.init = rng90_trng_init;
	trng->rng.read = rng90_trng_read;
	trng->rng.quality = 900;

	ret = devm_hwrng_register(dev, &trng->rng);
	if (ret) {
		dev_err(dev, "failed to register rng device: %d\n", ret);
		return ret;
	}

	return 0;
}

static int rng90_power_off(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	uint8_t pkt[1] = { RNG90_SLEEP };
	int ret;

	ret = i2c_master_send(client, pkt, sizeof(pkt));
	if (ret < 0) {
		dev_err(&client->dev, "RNG sleep failed: %d\n", ret);
	}

	dev_dbg(&client->dev, "RNG90 put to sleep\n");

	return 0;
}

static int rng90_power_on(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	uint8_t pkt[1] = { RNG90_RESET };
	uint8_t response[4];
	int ret;

	ret = i2c_master_send(client, pkt, sizeof(pkt));
	if (ret > 0) {
		dev_dbg(dev, "RNG was not sleeping\n");
	}

	msleep(RNG90_SELFTEST_MAX_WAIT_MS);

	ret = i2c_master_recv(client, response, sizeof(response));
	if (ret < 0 || !(response[0] == 0x4 && response[1] == 0x11 &&
			 response[2] == 0x33 && response[3] == 0x43)) {
		dev_err(dev, "RNG did not wake\n");
		return -EIO;
	}

	dev_dbg(&client->dev, "RNG90 taken out of sleep\n");

	return 0;
}

static const struct i2c_device_id rng90_id[] = {
	{ "rng90" },
	{},
};
MODULE_DEVICE_TABLE(i2c, rng90_id);

static const struct of_device_id of_rng90_match[] = {
	{ .compatible = "microchip,rng90" },
	{}
};
MODULE_DEVICE_TABLE(of, of_rng90_match);

static const struct dev_pm_ops rng90_pm_ops = { SET_RUNTIME_PM_OPS(
	rng90_power_off, rng90_power_on, NULL) };

static struct i2c_driver rng90_driver = {
	.driver = {
		.name = "rng90",
		.pm	= &rng90_pm_ops,
		.of_match_table = of_match_ptr(of_rng90_match),
	},
	.id_table = rng90_id,
	.probe = rng90_probe,
};
module_i2c_driver(rng90_driver);

MODULE_AUTHOR("Jared Baur <jaredbaur@fastmail.com>");
MODULE_DESCRIPTION("Microchip RNG90 driver");
MODULE_LICENSE("GPL v2");
