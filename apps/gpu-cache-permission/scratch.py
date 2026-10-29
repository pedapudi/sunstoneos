def permission_rom_words(transaction, evidence, record, save):
    """Protect FEL readl scratch before launch; transaction pins the same ROM instance.

    A transport failure aborts without another request. The caller retains the
    instance when scratch_dirty remains set. These bytes are never restored
    after native execution or over the new ROM instance.
    """
    before = evidence / 'before-rom-scratch.bin'
    restored = evidence / 'restored-rom-scratch.bin'
    transaction('read', '0x71000', 128, before)
    original = before.read_bytes()
    if len(original) != 128:
        raise ValueError('Short ROM scratch backup')
    expected = hashlib.sha256(original).hexdigest()
    record.update(scratch_dirty=True, scratch_restored=False, scratch_sha256=expected)
    save()
    words = transaction('readl', '0x20', 'readl', '0x24', 'readl', '0x28',
                        'readl', '0x3c', 'readl', '0x40')
    if before.read_bytes() != original:
        raise ValueError('ROM scratch backup changed')
    transaction('write', '0x71000', before, 'read', '0x71000', 128, restored)
    if restored.read_bytes() != original:
        raise ValueError('ROM scratch restoration differs')
    record.update(scratch_dirty=False, scratch_restored=True)
    save()
    return words
