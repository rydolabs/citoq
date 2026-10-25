import cito
def main():
    assert cito.abi_version() == 1
    received = []
    with cito.Context('acme') as ctx:
        sub = ctx.on(100, received.append, 'drone_17')
        assert ctx.publish(100, b'ignored', 'drone_18') == 0
        assert received == []
        assert ctx.publish(100, b'hello', 'drone_17') == 1
        assert received == [b'hello']
        sub.close()
        assert ctx.publish(100, b'after-close', 'drone_17') == 0
        ctx.run()
if __name__ == '__main__': main()
