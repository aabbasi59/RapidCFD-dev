/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | Copyright (C) 2011-2013 OpenFOAM Foundation
     \\/     M anipulation  |
-------------------------------------------------------------------------------
License
    This file is part of OpenFOAM.

    OpenFOAM is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM.  If not, see <http://www.gnu.org/licenses/>.

\*---------------------------------------------------------------------------*/

#include "GAMGAgglomeration.H"
#include "GAMGInterface.H"
#include "processorGAMGInterface.H"

// * * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * //

void Foam::GAMGAgglomeration::createSort
(
    const labelgpuList& list, 
    labelgpuList& sort
)
{
    sort.setSize(list.size());

    thrust::copy
    (
        thrust::make_counting_iterator(0),
        thrust::make_counting_iterator(0)+list.size(),
        sort.begin()
    );

    labelgpuList listTmp(list);
    thrust::stable_sort_by_key
    (
        listTmp.begin(),
        listTmp.end(),
        sort.begin()
    );
}

void Foam::GAMGAgglomeration::createTarget
(
    const labelgpuList& list,
    const labelgpuList& sort,
    labelgpuList& target,
    labelgpuList& targetStart
)
{
    labelgpuList ones(list.size(),1);
    labelgpuList tmpTarget(list.size());
    labelgpuList tmpSum(list.size());

    labelgpuList listSort(list.size());
    
    thrust::copy
    (
        thrust::make_permutation_iterator
        (
            list.begin(),
            sort.begin()
        ),
        thrust::make_permutation_iterator
        (
            list.begin(),
            sort.end()
        ),
        listSort.begin()
    );
 
    target = listSort;

    label targetSize = 
        thrust::unique
        (
            target.begin(),
            target.end()
        ) 
        - target.begin();
    target.setSize(targetSize);

    targetStart.setSize(list.size());
  
    thrust::reduce_by_key
    (
        listSort.begin(),
        listSort.end(),
        ones.begin(),
        tmpTarget.begin(),
        tmpSum.begin()
    );

    thrust::exclusive_scan
    (
        tmpSum.begin(),
        tmpSum.end(),
        targetStart.begin()
    );

    targetStart.setSize(targetSize+1);
    targetStart.set(targetSize,list.size());
}


void Foam::GAMGAgglomeration::agglomerateLduAddressing
(
    const label fineLevelIndex
)
{
    const lduMesh& fineMesh = meshLevel(fineLevelIndex);
    const lduAddressing& fineMeshAddr = fineMesh.lduAddr();

    const labelUList& upperAddr = fineMeshAddr.upperAddrHost();
    const labelUList& lowerAddr = fineMeshAddr.lowerAddrHost();

    label nFineFaces = upperAddr.size();

    // Get restriction map for current level
    const labelField& restrictMap = restrictAddressingHost(fineLevelIndex);

    if (min(restrictMap) == -1)
    {
        FatalErrorIn("GAMGAgglomeration::agglomerateLduAddressing")
            << "min(restrictMap) == -1" << exit(FatalError);
    }

    if (restrictMap.size() != fineMeshAddr.size())
    {
        FatalErrorIn
        (
            "GAMGAgglomeration::agglomerateLduAddressing"
            "(const label fineLevelIndex)"
        )   << "restrict map does not correspond to fine level. " << endl
            << " Sizes: restrictMap: " << restrictMap.size()
            << " nEqns: " << fineMeshAddr.size()
            << abort(FatalError);
    }


    // Get the number of coarse cells
    const label nCoarseCells = nCells_[fineLevelIndex];

    // Storage for coarse cell neighbours and coefficients

    // Guess initial maximum number of neighbours in coarse cell
    label maxNnbrs = 10;

    // Number of faces for each coarse-cell
    labelList cCellnFaces(nCoarseCells, 0);

    // Setup initial packed storage for coarse-cell faces
    labelList cCellFaces(maxNnbrs*nCoarseCells);

    // Create face-restriction addressing
    faceRestrictAddressingHost_.set(fineLevelIndex, new labelList(nFineFaces));
    labelList& faceRestrictAddr = faceRestrictAddressingHost_[fineLevelIndex];

    // Initial neighbour array (not in upper-triangle order)
    labelList initCoarseNeighb(nFineFaces);

    // Counter for coarse faces
    label& nCoarseFaces = nFaces_[fineLevelIndex];
    nCoarseFaces = 0;

    // Loop through all fine faces
    forAll(upperAddr, fineFacei)
    {
        label rmUpperAddr = restrictMap[upperAddr[fineFacei]];
        label rmLowerAddr = restrictMap[lowerAddr[fineFacei]];

        if (rmUpperAddr == rmLowerAddr)
        {
            // For each fine face inside of a coarse cell keep the address
            // of the cell corresponding to the face in the faceRestrictAddr
            // as a negative index
            faceRestrictAddr[fineFacei] = -(rmUpperAddr + 1);
        }
        else
        {
            // this face is a part of a coarse face

            label cOwn = rmUpperAddr;
            label cNei = rmLowerAddr;

            // get coarse owner and neighbour
            if (rmUpperAddr > rmLowerAddr)
            {
                cOwn = rmLowerAddr;
                cNei = rmUpperAddr;
            }

            // check the neighbour to see if this face has already been found
            label* ccFaces = &cCellFaces[maxNnbrs*cOwn];

            bool nbrFound = false;
            label& ccnFaces = cCellnFaces[cOwn];

            for (int i=0; i<ccnFaces; i++)
            {
                if (initCoarseNeighb[ccFaces[i]] == cNei)
                {
                    nbrFound = true;
                    faceRestrictAddr[fineFacei] = ccFaces[i];
                    break;
                }
            }

            if (!nbrFound)
            {
                if (ccnFaces >= maxNnbrs)
                {
                    label oldMaxNnbrs = maxNnbrs;
                    maxNnbrs *= 2;

                    cCellFaces.setSize(maxNnbrs*nCoarseCells);

                    forAllReverse(cCellnFaces, i)
                    {
                        label* oldCcNbrs = &cCellFaces[oldMaxNnbrs*i];
                        label* newCcNbrs = &cCellFaces[maxNnbrs*i];

                        for (int j=0; j<cCellnFaces[i]; j++)
                        {
                            newCcNbrs[j] = oldCcNbrs[j];
                        }
                    }

                    ccFaces = &cCellFaces[maxNnbrs*cOwn];
                }

                ccFaces[ccnFaces] = nCoarseFaces;
                initCoarseNeighb[nCoarseFaces] = cNei;
                faceRestrictAddr[fineFacei] = nCoarseFaces;
                ccnFaces++;

                // new coarse face created
                nCoarseFaces++;
            }
        }
    } // end for all fine faces

    // Renumber into upper-triangular order

    // All coarse owner-neighbour storage
    labelList coarseOwner(nCoarseFaces);
    labelList coarseNeighbour(nCoarseFaces);
    labelList coarseFaceMap(nCoarseFaces);

    label coarseFacei = 0;

    forAll(cCellnFaces, cci)
    {
        label* cFaces = &cCellFaces[maxNnbrs*cci];
        label ccnFaces = cCellnFaces[cci];

        for (int i=0; i<ccnFaces; i++)
        {
            coarseOwner[coarseFacei] = cci;
            coarseNeighbour[coarseFacei] = initCoarseNeighb[cFaces[i]];
            coarseFaceMap[cFaces[i]] = coarseFacei;
            coarseFacei++;
        }
    }

    forAll(faceRestrictAddr, fineFacei)
    {
        if (faceRestrictAddr[fineFacei] >= 0)
        {
            faceRestrictAddr[fineFacei] =
                coarseFaceMap[faceRestrictAddr[fineFacei]];
        }
    }

    // Create face-flip status
    faceFlipMapHost_.set(fineLevelIndex, new boolList(nFineFaces, false));
    boolList& faceFlipMap = faceFlipMapHost_[fineLevelIndex];


    label nFlipped = 0;
    label nDissapear = 0;

    forAll(faceRestrictAddr, fineFacei)
    {
        label coarseFacei = faceRestrictAddr[fineFacei];

        if (coarseFacei >= 0)
        {
            // Maps to coarse face
            label cOwn = coarseOwner[coarseFacei];
            label cNei = coarseNeighbour[coarseFacei];

            label rmUpperAddr = restrictMap[upperAddr[fineFacei]];
            label rmLowerAddr = restrictMap[lowerAddr[fineFacei]];

            if (cOwn == rmUpperAddr && cNei == rmLowerAddr)
            {
                faceFlipMap[fineFacei] = true;
                nFlipped++;
            }
            else if (cOwn == rmLowerAddr && cNei == rmUpperAddr)
            {
                //faceFlipMap[fineFacei] = false;
            }
            else
            {
                FatalErrorIn("GAMGAgglomeration::agglomerateLduAddressing(..)")
                    << "problem."
                    << " fineFacei:" << fineFacei
                    << " rmUpperAddr:" << rmUpperAddr
                    << " rmLowerAddr:" << rmLowerAddr
                    << " coarseFacei:" << coarseFacei
                    << " cOwn:" << cOwn
                    << " cNei:" << cNei
                    << exit(FatalError);
            }
        }
        else
        {
            nDissapear++;
        }
    }

    // Clear the temporary storage for the coarse cell data
    cCellnFaces.setSize(0);
    cCellFaces.setSize(0);
    initCoarseNeighb.setSize(0);
    coarseFaceMap.setSize(0);


    // Create coarse-level interfaces

    // Get reference to fine-level interfaces
    const lduInterfacePtrsList& fineInterfaces = interfaceLevel(fineLevelIndex);

    nPatchFaces_.set(fineLevelIndex, new labelList(fineInterfaces.size(), 0));
    labelList& nPatchFaces = nPatchFaces_[fineLevelIndex];

    patchFaceRestrictAddressingHost_.set
    (
        fineLevelIndex,
        new labelListList(fineInterfaces.size())
    );
    labelListList& patchFineToCoarse =
        patchFaceRestrictAddressingHost_[fineLevelIndex];


    // Initialise transfer of restrict addressing on the interface
    forAll(fineInterfaces, inti)
    {
        if (fineInterfaces.set(inti))
        {
            fineInterfaces[inti].initInternalFieldTransfer
            (
                Pstream::nonBlocking,
                restrictMap
            );
        }
    }

    if (Pstream::parRun())
    {
        Pstream::waitRequests();
    }

    // Add the coarse level
    meshLevels_.set
    (
        fineLevelIndex,
        new lduPrimitiveMesh
        (
            nCoarseCells,
            coarseOwner,
            coarseNeighbour,
            fineMesh.comm(),
            true
        )
    );
    
    lduInterfacePtrsList coarseInterfaces(fineInterfaces.size());

    forAll(fineInterfaces, inti)
    {
        if (fineInterfaces.set(inti))
        {
            coarseInterfaces.set
            (
                inti,
                GAMGInterface::New
                (
                    inti,
                    meshLevels_[fineLevelIndex].rawInterfaces(),
                    fineInterfaces[inti],
                    fineInterfaces[inti].interfaceInternalField(restrictMap),
                    fineInterfaces[inti].internalFieldTransfer
                    (
                        Pstream::nonBlocking,
                        restrictMap
                    ),
                    fineLevelIndex,
                    fineMesh.comm()
                ).ptr()
            );

            nPatchFaces[inti] = coarseInterfaces[inti].faceCells().size();
            patchFineToCoarse[inti] = refCast<const GAMGInterface>
            (
                coarseInterfaces[inti]
            ).faceRestrictAddressingHost();
        }
    }

    meshLevels_[fineLevelIndex].addInterfaces
    (
        coarseInterfaces,
        lduPrimitiveMesh::nonBlockingSchedule<processorGAMGInterface>
        (
            coarseInterfaces
        )
    );

    //GPU addressing
    patchFaceRestrictAddressing_.set
    (
        fineLevelIndex,
        new labelgpuListList(fineInterfaces.size())
    );
    patchFaceRestrictSortAddressing_.set
    ( 
        fineLevelIndex,
        new labelgpuListList(fineInterfaces.size())
    );
    patchFaceRestrictTargetAddressing_.set
    (
        fineLevelIndex,
        new labelgpuListList(fineInterfaces.size())
    );
    patchFaceRestrictTargetStartAddressing_.set
    ( 
        fineLevelIndex,
        new labelgpuListList(fineInterfaces.size())
    );

    labelgpuListList& patchFineToCoarseDevice =
        patchFaceRestrictAddressing_[fineLevelIndex];
    labelgpuListList& patchFineToCoarseSortDevice =
        patchFaceRestrictSortAddressing_[fineLevelIndex];
    labelgpuListList& patchFineToCoarseTargetDevice =
        patchFaceRestrictTargetAddressing_[fineLevelIndex];
    labelgpuListList& patchFineToCoarseTargetStartDevice =
        patchFaceRestrictTargetStartAddressing_[fineLevelIndex];

    forAll(patchFineToCoarse,patchi)
    {
        patchFineToCoarseDevice[patchi] = patchFineToCoarse[patchi];

        createSort
        (
            patchFineToCoarseDevice[patchi],
            patchFineToCoarseSortDevice[patchi]
        );

        createTarget
        (
            patchFineToCoarseDevice[patchi],
            patchFineToCoarseSortDevice[patchi],
            patchFineToCoarseTargetDevice[patchi],
            patchFineToCoarseTargetStartDevice[patchi]
        );
    }


    faceFlipMap_.set(fineLevelIndex, new boolgpuList(faceFlipMap));
    faceRestrictAddressing_.set(fineLevelIndex, new labelgpuList(faceRestrictAddr));
    faceRestrictSortAddressing_.set(fineLevelIndex, new labelgpuList(faceRestrictAddr.size()));

    createSort
    (
        faceRestrictAddressing_[fineLevelIndex],
        faceRestrictSortAddressing_[fineLevelIndex]
    );

    faceRestrictTargetAddressing_.set(fineLevelIndex, new labelgpuList());
    faceRestrictTargetStartAddressing_.set(fineLevelIndex, new labelgpuList());

    createTarget
    (
        faceRestrictAddressing_[fineLevelIndex],
        faceRestrictSortAddressing_[fineLevelIndex],
        faceRestrictTargetAddressing_[fineLevelIndex],
        faceRestrictTargetStartAddressing_[fineLevelIndex]
    );

    if (debug & 2)
    {
        Pout<< "GAMGAgglomeration :"
            << " agglomerated level " << fineLevelIndex
            << " from nCells:" << fineMeshAddr.size()
            << " nFaces:" << upperAddr.size()
            << " to nCells:" << nCoarseCells
            << " nFaces:" << nCoarseFaces
            << endl;
    }
}


void Foam::GAMGAgglomeration::procAgglomerateLduAddressing
(
    const label meshComm,
    const labelList& procAgglomMap,
    const labelList& procIDs,
    const label allMeshComm,

    const label levelIndex
)
{
    const lduMesh& myMesh = meshLevels_[levelIndex-1];


    label oldWarn = UPstream::warnComm;
    UPstream::warnComm = meshComm;


    procAgglomMap_.set(levelIndex, new labelList(procAgglomMap));
    agglomProcIDs_.set(levelIndex, new labelList(procIDs));
    procCommunicator_[levelIndex] = allMeshComm;

    // These could only be set on the master procs but it is
    // quite convenient to also have them on the slaves
    procCellOffsets_.set(levelIndex, new labelList(0));
    procFaceMapHost_.set(levelIndex, new labelListList(0));
    procBoundaryMap_.set(levelIndex, new labelListList(0));
    procBoundaryFaceMapHost_.set(levelIndex, new labelListListList(0));


    // Collect meshes
    PtrList<lduPrimitiveMesh> otherMeshes;
    lduPrimitiveMesh::gather(meshComm, myMesh, procIDs, otherMeshes);

    if (Pstream::myProcNo(meshComm) == procIDs[0])
    {
        // Combine all addressing

        labelList procFaceOffsets;
        meshLevels_.set
        (
            levelIndex-1,
            new lduPrimitiveMesh
            (
                allMeshComm,
                procAgglomMap,

                procIDs,
                myMesh,
                otherMeshes,

                procCellOffsets_[levelIndex],
                procFaceOffsets,
                procFaceMapHost_[levelIndex],
                procBoundaryMap_[levelIndex],
                procBoundaryFaceMapHost_[levelIndex]
            )
        );
    }


    // Combine restrict addressing
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~

    procAgglomerateRestrictAddressing
    (
        meshComm,
        procIDs,
        levelIndex
    );

    if (Pstream::myProcNo(meshComm) != procIDs[0])
    {
        clearLevel(levelIndex);
    }

    UPstream::warnComm = oldWarn;
}


void Foam::GAMGAgglomeration::procAgglomerateRestrictAddressing
(
    const label comm,
    const labelList& procIDs,
    const label levelIndex
)
{
    // Collect number of cells
    labelList nFineCells;
    gatherList
    (
        comm,
        procIDs,
        restrictAddressing_[levelIndex].size(),
        nFineCells
    );

    labelList offsets(nFineCells.size()+1);
    {
        offsets[0] = 0;
        forAll(nFineCells, i)
        {
            offsets[i+1] = offsets[i] + nFineCells[i];
        }
    }

    // Combine and renumber nCoarseCells
    labelList nCoarseCells;
    gatherList
    (
        comm,
        procIDs,
        nCells_[levelIndex],
        nCoarseCells
    );

    // (cell)restrictAddressing
    const globalIndex cellOffsetter(offsets);

    labelList procRestrictAddressing;
    cellOffsetter.gather
    (
        comm,
        procIDs,
        restrictAddressingHost_[levelIndex],
        procRestrictAddressing,

        UPstream::msgType(),
        Pstream::nonBlocking    //Pstream::scheduled
    );

    if (Pstream::myProcNo(comm) == procIDs[0])
    {
        labelList coarseCellOffsets(procIDs.size()+1);
        {
            coarseCellOffsets[0] = 0;
            forAll(procIDs, i)
            {
                coarseCellOffsets[i+1] = coarseCellOffsets[i]+nCoarseCells[i];
            }
        }

        nCells_[levelIndex] = coarseCellOffsets.last();

        // Renumber consecutively
        for (label procI = 1; procI < procIDs.size(); procI++)
        {
            SubList<label> procSlot
            (
                procRestrictAddressing,
                offsets[procI+1]-offsets[procI],
                offsets[procI]
            );
            forAll(procSlot, i)
            {
                procSlot[i] += coarseCellOffsets[procI];
            }
        }

        restrictAddressingHost_[levelIndex].transfer(procRestrictAddressing);
    }

    restrictAddressing_[levelIndex] = restrictAddressingHost_[levelIndex];
}

void Foam::GAMGAgglomeration::combineLevels(const label curLevel)
{
    label prevLevel = curLevel - 1;

    // Set the previous level nCells to the current
    nCells_[prevLevel] = nCells_[curLevel];
    nFaces_[prevLevel] = nFaces_[curLevel];

    // Map the restrictAddressing from the coarser level into the previous
    // finer level

    const labelList& curResAddr = restrictAddressingHost_[curLevel];
    labelList& prevResAddr = restrictAddressingHost_[prevLevel];

    const labelList& curFaceResAddr = faceRestrictAddressingHost_[curLevel];
    labelList& prevFaceResAddr = faceRestrictAddressingHost_[prevLevel];
    const boolList& curFaceFlipMap = faceFlipMapHost_[curLevel];
    boolList& prevFaceFlipMap = faceFlipMapHost_[prevLevel];

    forAll(prevFaceResAddr, i)
    {
        if (prevFaceResAddr[i] >= 0)
        {
            label fineFaceI = prevFaceResAddr[i];
            prevFaceResAddr[i] = curFaceResAddr[fineFaceI];
            prevFaceFlipMap[i] = curFaceFlipMap[fineFaceI];
        }
        else
        {
            label fineFaceI = -prevFaceResAddr[i] - 1;
            prevFaceResAddr[i] = -curResAddr[fineFaceI] - 1;
            prevFaceFlipMap[i] = curFaceFlipMap[fineFaceI];
        }
    }

    faceRestrictAddressing_[prevLevel] = prevFaceResAddr;
    faceFlipMap_[prevLevel] = prevFaceFlipMap;
    faceRestrictSortAddressing_.set(prevLevel, new labelgpuList(prevFaceResAddr.size()));

    createSort
    (
        faceRestrictAddressing_[prevLevel],
        faceRestrictSortAddressing_[prevLevel]
    );

    createTarget
    (
        faceRestrictAddressing_[prevLevel],
        faceRestrictSortAddressing_[prevLevel],
        faceRestrictTargetAddressing_[prevLevel],
        faceRestrictTargetStartAddressing_[prevLevel]
    );

    // Delete the restrictAddressing for the coarser level
    faceRestrictAddressingHost_.set(curLevel, NULL);
    faceFlipMapHost_.set(curLevel, NULL);

    faceRestrictAddressing_.set(curLevel, NULL);
    faceRestrictSortAddressing_.set(curLevel, NULL);
    faceRestrictTargetAddressing_.set(curLevel, NULL);
    faceRestrictTargetStartAddressing_.set(curLevel, NULL);
    faceFlipMap_.set(curLevel, NULL);

    forAll(prevResAddr, i)
    {
        prevResAddr[i] = curResAddr[prevResAddr[i]];
    }

    restrictAddressing_[prevLevel] = prevResAddr;
    restrictSortAddressing_.set(prevLevel, new labelgpuField(prevResAddr.size()));

    createSort
    (
        restrictAddressing_[prevLevel],
        restrictSortAddressing_[prevLevel]
    );

    createTarget
    (
        restrictAddressing_[prevLevel],
        restrictSortAddressing_[prevLevel],
        restrictTargetAddressing_[prevLevel],
        restrictTargetStartAddressing_[prevLevel]
    );

    // Delete the restrictAddressing for the coarser level
    restrictAddressing_.set(curLevel, NULL);
    restrictSortAddressing_.set(curLevel, NULL);
    restrictTargetAddressing_.set(curLevel, NULL);
    restrictTargetStartAddressing_.set(curLevel, NULL);
    restrictAddressingHost_.set(curLevel, NULL);


    const labelListList& curPatchFaceResAddrHost =
        patchFaceRestrictAddressingHost_[curLevel];
    labelListList& prevPatchFaceResAddrHost =
        patchFaceRestrictAddressingHost_[prevLevel];

    forAll(prevPatchFaceResAddrHost, inti)
    {
        const labelList& curPatchResAddr = curPatchFaceResAddrHost[inti];
        labelList& prevPatchResAddr = prevPatchFaceResAddrHost[inti];
        forAll(prevPatchResAddr, i)
        {
            label fineFaceI = prevPatchResAddr[i];
            prevPatchResAddr[i] = curPatchResAddr[fineFaceI];
        }

        patchFaceRestrictAddressing_[prevLevel][inti] = prevPatchResAddr;

        createSort
        (
            patchFaceRestrictAddressing_[prevLevel][inti],
            patchFaceRestrictSortAddressing_[prevLevel][inti]
        );

        createTarget
        (
            patchFaceRestrictAddressing_[prevLevel][inti],
            patchFaceRestrictSortAddressing_[prevLevel][inti],
            patchFaceRestrictTargetAddressing_[prevLevel][inti],
            patchFaceRestrictTargetStartAddressing_[prevLevel][inti]
        );
    }

    // Patch faces
    nPatchFaces_[prevLevel] = nPatchFaces_[curLevel];

    // Adapt the restrict addressing for the patches
    const lduInterfacePtrsList& curInterLevel =
        meshLevels_[curLevel].rawInterfaces();
    const lduInterfacePtrsList& prevInterLevel =
        meshLevels_[prevLevel].rawInterfaces();

    forAll(prevInterLevel, inti)
    {
        if (prevInterLevel.set(inti))
        {
            GAMGInterface& prevInt = refCast<GAMGInterface>
            (
                const_cast<lduInterface&>
                (
                    prevInterLevel[inti]
                )
            );
            const GAMGInterface& curInt = refCast<const GAMGInterface>
            (
                curInterLevel[inti]
            );
            prevInt.combine(curInt);
        }
    }

    // Delete the matrix addressing and coefficients from the previous level
    // and replace with the corresponding entry from the coarser level
    meshLevels_.set(prevLevel, meshLevels_.set(curLevel, NULL));
}


//void Foam::GAMGAgglomeration::gatherList
//(
//    const label comm,
//    const labelList& procIDs,
//
//    const label myVal,
//    labelList& vals,
//    const int tag
//)
//{
//    vals.setSize(procIDs.size());
//
//    if (Pstream::myProcNo(comm) == procIDs[0])
//    {
//        vals[0] = myVal;
//
//        for (label i = 1; i < procIDs.size(); i++)
//        {
//            label& slaveVal = vals[i];
//            IPstream::read
//            (
//                Pstream::scheduled,
//                procIDs[i],
//                reinterpret_cast<char*>(&slaveVal),
//                sizeof(slaveVal),
//                tag,
//                comm
//            );
//        }
//    }
//    else
//    {
//        OPstream::write
//        (
//            Pstream::scheduled,
//            procIDs[0],
//            reinterpret_cast<const char*>(&myVal),
//            sizeof(myVal),
//            tag,
//            comm
//        );
//    }
//}


void Foam::GAMGAgglomeration::calculateRegionMaster
(
    const label comm,
    const labelList& procAgglomMap,
    labelList& masterProcs,
    List<int>& agglomProcIDs
)
{
    // Determine the master processors
    Map<label> agglomToMaster(procAgglomMap.size());

    forAll(procAgglomMap, procI)
    {
        label coarseI = procAgglomMap[procI];

        Map<label>::iterator fnd = agglomToMaster.find(coarseI);
        if (fnd == agglomToMaster.end())
        {
            agglomToMaster.insert(coarseI, procI);
        }
        else
        {
            fnd() = min(fnd(), procI);
        }
    }

    masterProcs.setSize(agglomToMaster.size());
    forAllConstIter(Map<label>, agglomToMaster, iter)
    {
        masterProcs[iter.key()] = iter();
    }


    // Collect all the processors in my agglomeration
    label myProcID = Pstream::myProcNo(comm);
    label myAgglom = procAgglomMap[myProcID];

    // Get all processors agglomerating to the same coarse
    // processor
    agglomProcIDs = findIndices(procAgglomMap, myAgglom);
    // Make sure the master is the first element.
    label index = findIndex
    (
        agglomProcIDs,
        agglomToMaster[myAgglom]
    );
    Swap(agglomProcIDs[0], agglomProcIDs[index]);
}


// ************************************************************************* //
