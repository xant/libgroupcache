package shardcache;

import java.io.*;
import java.lang.reflect.Array;
import java.net.*;
import java.util.Arrays;

import static shardcache.ShardcacheMessage.*;

public class ShardcacheClient {
	private ShardcacheNode[] nodes;
	private CHash cHash;

	public ShardcacheClient(ShardcacheNode[] nodes) {
		this.nodes = nodes;
		String[] nodeNames = new String[nodes.length];
		for (int i = 0; i < nodes.length; i++)
			nodeNames[i] = nodes[i].getLabel();
		this.cHash = new CHash(nodeNames, 200);
	}
	
	public ShardcacheClient(String nodesString) {
		String[] substrings = nodesString.split(",");
		ShardcacheNode[] nodes = new ShardcacheNode[substrings.length];
		String[] nodeNames = new String[substrings.length];
		
		for (int i = 0; i < substrings.length; i++) {
			String str = substrings[i];
			String[] components = str.split(":");
			
			assert(components.length == 3) : 
				"Node string must be of the form <label>:<address>:<port>. " +
				"All fields are mandatory";
			
			String label = components[0];
			String address = components[1];
			String port = components[2];
			
			ShardcacheNode node = new ShardcacheNode(label, address, port);
			nodes[i] = node;
			nodeNames[i] = label;
		}
		this.cHash = new CHash(nodeNames, 100);
		this.nodes = nodes;
	}
	
	ShardcacheNode[] getNodes() {
		return this.nodes;
	}
	
/*
	private String getNodeAddress(String nodeLabel) {
		for (Node node : this.nodes) {
			if (node.getLabel() == nodeLabel)
				return node.getFullAddress();
		}
		return null;
	}
	
	private String getNodeLabel(String nodeAddress) {
		for (Node node : this.nodes) {
			if (node.getFullAddress() == nodeAddress)
				return node.getLabel();
		}
		return null;
	}
*/
	private ShardcacheNode selectNode(String key) {
		String label = this.cHash.lookup(key);
		for (ShardcacheNode node: nodes) {
			if (node.getLabel().equals(label)) {
				return node;
			}
		}
		return null;
	}
	
	public byte[] get(String key) {
		ShardcacheNode owner = this.selectNode(key);

		
		int numChunks = (key.length() / 65536) + 1;
		byte[] request = new byte[4 + 1 + 2 * numChunks + key.length() + 2 + 1];
		
		byte[] data = key.getBytes();
		int doffset = 0;

		ShardcacheMessage.Builder builder = new ShardcacheMessage.Builder();

        builder.setMessageType(Type.GET);

        builder.addRecord(key.getBytes());

        ShardcacheMessage message = builder.build();


		ShardcacheConnection connection = owner.connect();
		if (connection == null)
			return null;

		connection.send(message);
        ShardcacheMessage response = connection.receive();

        ShardcacheMessage.Record record = response.recordAtIndex(0);
		return record.getData();
	}

    public int set(String key, byte[] data) {
        ShardcacheNode owner = this.selectNode(key);
        ShardcacheMessage.Builder builder = new ShardcacheMessage.Builder();
        builder.setMessageType(Type.SET);
        builder.addRecord(key.getBytes());
        builder.addRecord(data);

        ShardcacheMessage message = builder.build();

        ShardcacheConnection connection = owner.connect();
        if (connection == null)
            return -1;

        connection.send(message);
        ShardcacheMessage response = connection.receive();

        if (response.hdr == HDR_RESPONSE) {
            Record r = response.recordAtIndex(0);
            byte[] rd = r.getData();
            if (rd.length == 1 && rd[0] == 0x00)
                return 0;
        }
        return -1;
    }
	
}
