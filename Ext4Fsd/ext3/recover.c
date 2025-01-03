/*
 * COPYRIGHT:        See COPYRIGHT.TXT
 * PROJECT:          Ext2 File System Driver for WinNT/2K/XP
 * FILE:             recover.c
 * PROGRAMMER:       Matt Wu <mattwu@163.com>
 * HOMEPAGE:         http://www.ext2fsd.com
 * UPDATE HISTORY:
 */

/* INCLUDES *****************************************************************/

#include <ext2fs.h>
#include <linux/jbd2.h>

/* GLOBALS ***************************************************************/

extern PEXT2_GLOBAL Ext2Global;

/* DEFINITIONS *************************************************************/

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2LoadInternalJournal)
#pragma alloc_text(PAGE, Ext2CheckJournal)
#pragma alloc_text(PAGE, Ext2RecoverJournal)
#endif

PEXT2_MCB
Ext2LoadInternalJournal(
    PEXT2_VCB         Vcb,
    ULONG             jNo
)
{
    PEXT2_MCB   Jcb = NULL;

    Jcb = Ext2AllocateMcb(Vcb, NULL, NULL, 0);
    if (!Jcb) {
        goto errorout;
    }

    Jcb->Inode.i_ino = jNo;
    Jcb->Inode.i_sb = &Vcb->sb;
    if (!Ext2LoadInode(Vcb, &Jcb->Inode)) {
        DbgBreak();
        Ext2FreeMcb(Vcb, Jcb);
        goto errorout;
    }

errorout:

    return Jcb;
}

INT
Ext2CheckJournal(
    PEXT2_VCB          Vcb,
    PULONG             jNo
)
{
    struct ext3_super_block* esb = NULL;

    /* check ext3 super block */
    esb = (struct ext3_super_block *)Vcb->SuperBlock;
    if (IsFlagOn(esb->s_feature_incompat,
                 EXT3_FEATURE_INCOMPAT_RECOVER)) {
        SetLongFlag(Vcb->Flags, VCB_JOURNAL_RECOVER);
    }

    /* must stop here if volume is read-only */
    if (IsVcbReadOnly(Vcb)) {
        goto errorout;
    }

    /* journal is external ? */
    if (esb->s_journal_inum == 0) {
        goto errorout;
    }

    /* oops: volume is corrupted */
    if (esb->s_journal_dev) {
        goto errorout;
    }

    /* return the journal inode number */
    *jNo = esb->s_journal_inum;

    return TRUE;

errorout:

    return FALSE;
}

INT
Ext2RecoverJournal(
    PEXT2_IRP_CONTEXT  IrpContext,
    PEXT2_VCB          Vcb
)
{
    INT rc = 0;
    ULONG                   jNo = 0;
    PEXT2_MCB               jcb = NULL;
    struct block_device *   bd = &Vcb->bd;
    struct super_block *    sb = &Vcb->sb;
    struct inode *          ji = NULL;
    journal_t *             journal = NULL;
    struct ext3_super_block *esb;

    ExAcquireResourceExclusiveLite(&Vcb->MainResource, TRUE);

    /* check journal inode number */
    if (!Ext2CheckJournal(Vcb, &jNo)) {
        rc = -1;
        goto errorout;
    }

    /* allocate journal Mcb */
    jcb =  Ext2LoadInternalJournal(Vcb, jNo);
    if (!jcb) {
        rc = -6;
        goto errorout;
    }

    /* allocate journal inode */
    ji = &jcb->Inode;

    /* initialize journal file from inode */
    journal = jbd2_journal_init_inode(ji);

    /* initialzation succeeds ? */
    if (!journal) {
        DEBUG(DL_ERR, ( "jbd2_journal_init_inode failed\n"));
        iput(ji);
        rc = -8;
        goto errorout;
    }

	if (ext4_has_feature_journal_needs_recovery(sb)) {
        /* loading the journal will do a recover */
		rc = jbd2_journal_load(journal);

        if (0 != rc) {
            DEBUG(DL_ERR, ( "Ext2Fsd: recover_journal: failed "
                 "to recover journal data. rc=%d\n", rc));
            rc = -9;
            //goto errorout;
        }

        /* reload super_block and group_description */
        Ext2RefreshSuper(IrpContext, Vcb);
        Ext2RefreshGroup(IrpContext, Vcb);

        /* clear recover flag in sb */
        if (rc == 0) {
            ClearLongFlag(
                Vcb->SuperBlock->s_feature_incompat,
                EXT3_FEATURE_INCOMPAT_RECOVER);
            Ext2SaveSuper(IrpContext, Vcb);
            sync_blockdev(bd);
            ClearLongFlag(Vcb->Flags, VCB_JOURNAL_RECOVER);
        }
    }
    else {
        /* if the journal is clean wipe it */
		rc = jbd2_journal_wipe(journal, !IsVcbReadOnly(Vcb));
    }

errorout:

    /* destroy journal structure */
    if (journal) {
        jbd2_journal_destroy(journal);
    }

    /* destory journal Mcb */
    if (jcb) {
        Ext2FreeMcb(Vcb, jcb);
    }

    ExReleaseResourceLite(&Vcb->MainResource);

    return rc;
}
