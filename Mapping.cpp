#include <cmath>
#include <sys/time.h>
#include "structure.h"

#define MAPQ_COEF 30
#define Max_MAPQ  50

gzFile gzOutput;
//char buffer[4096];
FILE *output = stdout;
time_t StartProcessTime;
bool bSepLibrary = false;
FILE *ReadFileHandler1, *ReadFileHandler2;
gzFile gzReadFileHandler1, gzReadFileHandler2;
static pthread_mutex_t LibraryLock, OutputLock;
map<pair<int64_t, int64_t>, SpliceJunction_t> SpliceJunctionMap;
int iTotalReadNum = 0, iUniqueMapped = 0, iUnMapped = 0, iPaired = 0;

void ShowAlignmentCandidateInfo(bool bFirstRead, char* header, vector<AlignmentCandidate_t>& AlignmentVec)
{
	vector<AlignmentCandidate_t>::iterator iter;

	printf("\n%s\n", string().assign(100, '-').c_str());

	printf("Alignment Candidate for read %s /%d\n", header, bFirstRead?1:2);
	for (iter = AlignmentVec.begin(); iter != AlignmentVec.end(); iter++)
	{
		if (iter->Score == 0) continue;
		printf("\tcandidate#%d: Score=%d\n", (int)(iter - AlignmentVec.begin() + 1), iter->Score);
		ShowSeedInfo(iter->SeedVec);
	}
	printf("%s\n\n", string().assign(100, '-').c_str());

	fflush(stdout);
}

bool CompByCandidateScore(const AlignmentCandidate_t& p1, const AlignmentCandidate_t& p2)
{
	if (p1.Score == p2.Score) return p1.PosDiff < p2.PosDiff;
	else return p1.Score > p2.Score;
}

void SetSingleAlignmentFlag(ReadItem_t& read)
{
	int i;

	if (read.score > read.sub_score) // unique mapping or bMultiHit=false
	{
		i = read.iBestAlnCanIdx;
		if (read.AlnReportArr[i].coor.bDir == false) read.AlnReportArr[i].iFrag = 0x10;
		else read.AlnReportArr[i].iFrag = 0;
	}
	else if (read.score > 0)
	{
		for (i = 0; i < read.CanNum; i++)
		{
			if (read.AlnReportArr[i].AlnScore > 0)
			{
				if (read.AlnReportArr[i].coor.bDir == false) read.AlnReportArr[i].iFrag = 0x10;
				else read.AlnReportArr[i].iFrag = 0;
			}
		}
	}
	else
	{
		read.AlnReportArr[0].iFrag = 0x4;
	}
}

void SetPairedAlignmentFlag(ReadItem_t& read1, ReadItem_t& read2)
{
	int i, j;

	//printf("read1:[%d, %d]:#%d, read2:[%d, %d] #%d\n", read1.score, read1.sub_score, read1.iBestAlnCanIdx, read2.score, read2.sub_score, read2.iBestAlnCanIdx); fflush(stdout);
	if (read1.score > read1.sub_score && read2.score > read2.sub_score) // unique mapping
	{
		i = read1.iBestAlnCanIdx;
		read1.AlnReportArr[i].iFrag = 0x41; // read1 is the first read in a pair

		j = read2.iBestAlnCanIdx;
		read2.AlnReportArr[j].iFrag = 0x81; // read2 is the second read in a pair

		if (j == read1.AlnReportArr[i].PairedAlnCanIdx) // reads are mapped in a proper pair
		{
			read1.AlnReportArr[i].iFrag |= 0x2;
			read2.AlnReportArr[j].iFrag |= 0x2;
		}
		read1.AlnReportArr[i].iFrag |= (read1.AlnReportArr[i].coor.bDir ? 0x20 : 0x10);
		read2.AlnReportArr[j].iFrag |= (read2.AlnReportArr[j].coor.bDir ? 0x20 : 0x10);
	}
	else
	{
		if (read1.score > read1.sub_score) // unique mapping
		{
			i = read1.iBestAlnCanIdx;
			read1.AlnReportArr[i].iFrag = 0x41; // read1 is the first read in a pair
			read1.AlnReportArr[i].iFrag |= (read1.AlnReportArr[i].coor.bDir ? 0x20 : 0x10);
			if ((j = read1.AlnReportArr[i].PairedAlnCanIdx) != -1 && read2.AlnReportArr[j].AlnScore > 0) read1.AlnReportArr[i].iFrag |= 0x2;// reads are mapped in a proper pair
			else read1.AlnReportArr[i].iFrag |= 0x8; // next segment unmapped
		}
		else if (read1.score > 0)
		{
			for (i = 0; i < read1.CanNum; i++)
			{
				if (read1.AlnReportArr[i].AlnScore > 0)
				{
					read1.AlnReportArr[i].iFrag = 0x41; // read1 is the first read in a pair
					read1.AlnReportArr[i].iFrag |= (read1.AlnReportArr[i].coor.bDir ? 0x20 : 0x10);

					if ((j = read1.AlnReportArr[i].PairedAlnCanIdx) != -1 && read2.AlnReportArr[j].AlnScore > 0) read1.AlnReportArr[i].iFrag |= 0x2;// reads are mapped in a proper pair
					else read1.AlnReportArr[i].iFrag |= 0x8; // next segment unmapped
				}
			}
		}
		else
		{
			read1.AlnReportArr[0].iFrag = 0x41; // read1 is the first read in a pair
			read1.AlnReportArr[0].iFrag |= 0x4;

			if (read2.score == 0) read1.AlnReportArr[0].iFrag |= 0x8; // next segment unmapped
			else read1.AlnReportArr[0].iFrag |= (read2.AlnReportArr[read2.iBestAlnCanIdx].coor.bDir ? 0x10 : 0x20);
		}

		if (read2.score > read2.sub_score) // unique mapping
		{
			j = read2.iBestAlnCanIdx;
			read2.AlnReportArr[j].iFrag = 0x81; // read2 is the second read in a pair
			read2.AlnReportArr[j].iFrag |= (read2.AlnReportArr[j].coor.bDir ? 0x20 : 0x10);

			if ((i = read2.AlnReportArr[j].PairedAlnCanIdx) != -1 && read1.AlnReportArr[i].AlnScore > 0) read2.AlnReportArr[j].iFrag |= 0x2;// reads are mapped in a proper pair
			else read2.AlnReportArr[j].iFrag |= 0x8; // next segment unmapped
		}
		else if (read2.score > 0)
		{
			for (j = 0; j < read2.CanNum; j++)
			{
				if (read2.AlnReportArr[j].AlnScore > 0)
				{
					read2.AlnReportArr[j].iFrag = 0x81; // read2 is the second read in a pair
					read2.AlnReportArr[j].iFrag |= (read2.AlnReportArr[j].coor.bDir ? 0x20 : 0x10);

					if ((i = read2.AlnReportArr[j].PairedAlnCanIdx) != -1 && read1.AlnReportArr[i].AlnScore > 0) read2.AlnReportArr[j].iFrag |= 0x2;// reads are mapped in a proper pair
					else read2.AlnReportArr[j].iFrag |= 0x8; // next segment unmapped
				}
			}
		}
		else
		{
			read2.AlnReportArr[0].iFrag = 0x81; // read2 is the second read in a pair
			read2.AlnReportArr[0].iFrag |= 0x4; // segment unmapped
			if (read1.score == 0) read2.AlnReportArr[0].iFrag |= 0x8; // next segment unmapped
			else read2.AlnReportArr[0].iFrag |= (read1.AlnReportArr[read1.iBestAlnCanIdx].coor.bDir ? 0x10 : 0x20);
		}
	}
}

void EvaluateMAPQ(ReadItem_t& read)
{
	int i, iMap;

	if (read.score == 0 || read.score == read.sub_score) read.mapq = 0;
	else
	{
		if (read.sub_score == 0 || read.score > read.sub_score) read.mapq = Max_MAPQ;
		else
		{
			for (iMap = 0, i = 0; i < read.CanNum; i++) if (read.AlnReportArr[i].AlnScore == read.score) iMap++;
			if (iMap >= 10) read.mapq = 0;
			else if (iMap >= 4) read.mapq = 1;
			else if (iMap == 3) read.mapq = 2;
			else if (iMap == 2) read.mapq = 3;
			else read.mapq = Max_MAPQ;

			//if(iMap < 1) read.mapq = (int)(-10 * log10(1 - (1.0 / iMap)));
			//else read.mapq = 0;
		}
	}
}

void OutputPairedAlignments(ReadItem_t& read1, ReadItem_t& read2)
{
	char *seq, *rseq;
	char* buffer = NULL;
	int i, j, len, dist = 0;

	buffer = (char*)malloc((200 + read1.rlen + (read1.rlen >> 1)));
	if (read1.score == 0)
	{
		iUnMapped++;
		len = sprintf(buffer, "%s\t%d\t*\t0\t0\t*\t*\t0\t0\t%s\t*\tAS:i:0\tXS:i:0\n", read1.header, read1.AlnReportArr[0].iFrag, read1.seq);

		if (OutputFileFormat == 0) fprintf(output, "%s", buffer);
		else gzwrite(gzOutput, buffer, len);
	}
	else
	{
		if (read1.mapq == Max_MAPQ) iUniqueMapped++;

		seq = read1.seq; rseq = NULL;
		for (i = read1.iBestAlnCanIdx; i < read1.CanNum; i++)
		{
			if (read1.AlnReportArr[i].AlnScore > 0)
			{
				if (read1.AlnReportArr[i].coor.bDir == false && rseq == NULL)
				{
					rseq = new char[read1.rlen + 1]; rseq[read1.rlen] = '\0';
					GetComplementarySeq(read1.rlen, seq, rseq);
				}
				if ((j = read1.AlnReportArr[i].PairedAlnCanIdx) != -1 && read2.AlnReportArr[j].AlnScore > 0)
				{
					dist = (int)(read2.AlnReportArr[j].coor.gPos - read1.AlnReportArr[i].coor.gPos + (read1.AlnReportArr[i].coor.bDir ? read2.rlen : 0 - read1.rlen));
					if (i == read1.iBestAlnCanIdx) iPaired += 2;
					len = sprintf(buffer, "%s\t%d\t%s\t%ld\t%d\t%s\t=\t%ld\t%d\t%s\t*\tNM:i:%d\tAS:i:%d\tXS:i:%d\n", read1.header, read1.AlnReportArr[i].iFrag, ChromosomeVec[read1.AlnReportArr[i].coor.ChromosomeIdx].name, read1.AlnReportArr[i].coor.gPos, read1.mapq, read1.AlnReportArr[i].coor.CIGAR.c_str(), read2.AlnReportArr[j].coor.gPos, dist, (read1.AlnReportArr[i].coor.bDir ? seq : rseq), read1.rlen - read1.score, read1.score, read1.sub_score);
				}
				else len = sprintf(buffer, "%s\t%d\t%s\t%ld\t%d\t%s\t*\t0\t0\t%s\t*\tNM:i:%d\tAS:i:%d\tXS:i:%d\n", read1.header, read1.AlnReportArr[i].iFrag, ChromosomeVec[read1.AlnReportArr[i].coor.ChromosomeIdx].name, read1.AlnReportArr[i].coor.gPos, read1.mapq, read1.AlnReportArr[i].coor.CIGAR.c_str(), (read1.AlnReportArr[i].coor.bDir ? seq : rseq), read1.rlen - read1.score, read1.score, read1.sub_score);

				if (OutputFileFormat == 0) fprintf(output, "%s", buffer);
				else gzwrite(gzOutput, buffer, len);;
			}
			if (!bMultiHit) break;
		}
		if (rseq != NULL)
		{
			delete[] rseq;
			rseq = NULL;
		}
	}
	free(buffer);

	buffer = (char*)malloc((200 + read2.rlen + (read2.rlen >> 1)));
	if (read2.score == 0)
	{
		iUnMapped++;
		len = sprintf(buffer, "%s\t%d\t*\t0\t0\t*\t*\t0\t0\t%s\t*\tAS:i:0\tXS:i:0\n", read2.header, read2.AlnReportArr[0].iFrag, read2.seq);

		if (OutputFileFormat == 0) fprintf(output, "%s", buffer);
		else gzwrite(gzOutput, buffer, len);;
	}
	else
	{
		if (read2.mapq == Max_MAPQ) iUniqueMapped++;

		rseq = read2.seq; seq = NULL;
		for (j = read2.iBestAlnCanIdx; j < read2.CanNum; j++)
		{
			if (read2.AlnReportArr[j].AlnScore > 0)
			{
				if (read2.AlnReportArr[j].coor.bDir == true && seq == NULL)
				{
					seq = new char[read2.rlen + 1]; seq[read2.rlen] = '\0';
					GetComplementarySeq(read2.rlen, rseq, seq);
				}
				if ((i = read2.AlnReportArr[j].PairedAlnCanIdx) != -1 && read1.AlnReportArr[i].AlnScore > 0)
				{
					dist = 0 - ((int)(read2.AlnReportArr[j].coor.gPos - read1.AlnReportArr[i].coor.gPos + (read1.AlnReportArr[i].coor.bDir ? read2.rlen : 0 - read1.rlen)));
					len = sprintf(buffer, "%s\t%d\t%s\t%ld\t%d\t%s\t=\t%ld\t%d\t%s\t*\tNM:i:%d\tAS:i:%d\tXS:i:%d\n", read2.header, read2.AlnReportArr[j].iFrag, ChromosomeVec[read2.AlnReportArr[j].coor.ChromosomeIdx].name, read2.AlnReportArr[j].coor.gPos, read2.mapq, read2.AlnReportArr[j].coor.CIGAR.c_str(), read1.AlnReportArr[i].coor.gPos, dist, (read2.AlnReportArr[j].coor.bDir ? seq : rseq), read2.rlen - read2.score, read2.score, read2.sub_score);
				}
				else len = sprintf(buffer, "%s\t%d\t%s\t%ld\t%d\t%s\t*\t0\t0\t%s\t*\tNM:i:%d\tAS:i:%d\tXS:i:%d\n", read2.header, read2.AlnReportArr[j].iFrag, ChromosomeVec[read2.AlnReportArr[j].coor.ChromosomeIdx].name, read2.AlnReportArr[j].coor.gPos, read2.mapq, read2.AlnReportArr[j].coor.CIGAR.c_str(), (read2.AlnReportArr[j].coor.bDir ? seq : rseq), read2.rlen - read2.score, read2.score, read2.sub_score);

				if (OutputFileFormat == 0) fprintf(output, "%s", buffer);
				else gzwrite(gzOutput, buffer, len);;
			}
			if (!bMultiHit) break;
		}
		if (seq != NULL)
		{
			delete[] seq;
			seq = NULL;
		}
	}
	free(buffer);
}

void OutputSingledAlignments(ReadItem_t& read)
{
	int len;
	char* buffer = NULL;

	buffer = (char*)malloc((200 + read.rlen + (read.rlen >> 1)));

	if (read.score == 0)
	{
		iUnMapped++;
		len = sprintf(buffer, "%s\t%d\t*\t0\t0\t*\t*\t0\t0\t%s\t*\tAS:i:0\tXS:i:0\n", read.header, read.AlnReportArr[0].iFrag, read.seq);

		if (OutputFileFormat == 0) fprintf(output, "%s", buffer);
		else gzwrite(gzOutput, buffer, len);;
	}
	else
	{
		int i;
		char *seq, *rseq;

		if (read.mapq == Max_MAPQ) iUniqueMapped++;

		seq = read.seq; rseq = NULL;
		for (i = read.iBestAlnCanIdx; i < read.CanNum; i++)
		{
			if (read.AlnReportArr[i].AlnScore == read.score)
			{
				if (read.AlnReportArr[i].coor.bDir == false && rseq == NULL)
				{
					rseq = new char[read.rlen + 1]; rseq[read.rlen] = '\0';
					GetComplementarySeq(read.rlen, seq, rseq);
				}
				len = sprintf(buffer, "%s\t%d\t%s\t%ld\t%d\t%s\t*\t0\t0\t%s\t*\tNM:i:%d\tAS:i:%d\tXS:i:%d\n", read.header, read.AlnReportArr[i].iFrag, ChromosomeVec[read.AlnReportArr[i].coor.ChromosomeIdx].name, read.AlnReportArr[i].coor.gPos, read.mapq, read.AlnReportArr[i].coor.CIGAR.c_str(), (read.AlnReportArr[i].coor.bDir ? seq : rseq), read.rlen - read.score, read.score, read.sub_score);

				if (OutputFileFormat == 0) fprintf(output, "%s", buffer);
				else gzwrite(gzOutput, buffer, len);;

				if (!bMultiHit) break;
			}
		}
		if (rseq != NULL)
		{
			delete[] rseq;
			rseq = NULL;
		}
	}
	free(buffer);
}

void RemoveRedundantCandidates(vector<AlignmentCandidate_t>& AlignmentVec)
{
	int thr, score1, score2;
	vector<AlignmentCandidate_t>::iterator iter;

	if (AlignmentVec.size() <= 1) return;
	else
	{
		score1 = score2 = 0;
		for (iter = AlignmentVec.begin(); iter != AlignmentVec.end(); iter++)
		{
			if (iter->Score > score2)
			{
				if (iter->Score >= score1)
				{
					score2 = score1;
					score1 = iter->Score;
				}
				else score2 = iter->Score;
			}
			else if (iter->Score == score2) score2 = score1;
		}
		//if (score1 == score2) bAmbiguous = true;

		if (score1 == score2 || score1 - score2 > 20) thr = score1;
		else thr = score2;

		if (bDebugMode) printf("Candidate score threshold = %d\n", thr);
		for (iter = AlignmentVec.begin(); iter != AlignmentVec.end(); iter++) if (iter->Score < thr) iter->Score = 0;
	}
}

bool CheckPairedAlignmentCandidates(vector<AlignmentCandidate_t>& AlignmentVec1, vector<AlignmentCandidate_t>& AlignmentVec2)
{
	bool bPairing = false;
	int64_t dist, min_dist;
	int i, j, best_mate, num1, num2;

	num1 = (int)AlignmentVec1.size(); num2 = (int)AlignmentVec2.size();

	if (num1*num2 > 1000)
	{
		RemoveRedundantCandidates(AlignmentVec1);
		RemoveRedundantCandidates(AlignmentVec2);
	}
	for (i = 0; i != num1; i++)
	{
		if (AlignmentVec1[i].Score == 0) continue;

		for (best_mate = -1, min_dist = 2000000, j = 0; j != num2; j++)
		{
			if (AlignmentVec2[j].Score == 0 || AlignmentVec2[j].PosDiff < AlignmentVec1[i].PosDiff) continue;

			dist = abs(AlignmentVec2[j].PosDiff - AlignmentVec1[i].PosDiff);
			//printf("#%d (s=%d) and #%d (s=%d) (dist=%ld)\n", i+1, AlignmentVec1[i].Score, j+1, AlignmentVec2[j].Score, dist), fflush(stdout);
			if (dist < min_dist)
			{
				best_mate = j;
				min_dist = dist;
			}
		}
		if (best_mate != -1)
		{
			j = best_mate;
			if (AlignmentVec2[j].PairedAlnCanIdx == -1)
			{
				bPairing = true;
				AlignmentVec1[i].PairedAlnCanIdx = j;
				AlignmentVec2[j].PairedAlnCanIdx = i;
			}
			else if (AlignmentVec1[i].Score > AlignmentVec1[AlignmentVec2[j].PairedAlnCanIdx].Score)
			{
				AlignmentVec1[AlignmentVec2[j].PairedAlnCanIdx].PairedAlnCanIdx = -1;
				AlignmentVec1[i].PairedAlnCanIdx = j;
				AlignmentVec2[j].PairedAlnCanIdx = i;
			}
		}
	}
	return bPairing;
}

void RemoveUnMatedAlignmentCandidates(vector<AlignmentCandidate_t>& AlignmentVec1, vector<AlignmentCandidate_t>& AlignmentVec2)
{
	int i, j, num1, num2;

	num1 = (int)AlignmentVec1.size(); num2 = (int)AlignmentVec2.size();

	for (i = 0; i != num1; i++)
	{
		if (AlignmentVec1[i].PairedAlnCanIdx == -1) AlignmentVec1[i].Score = 0;
		else
		{
			j = AlignmentVec1[i].PairedAlnCanIdx;
			AlignmentVec1[i].Score = AlignmentVec2[j].Score = AlignmentVec1[i].Score + AlignmentVec2[j].Score;
		}
	}
	for (j = 0; j != num2; j++) if (AlignmentVec2[j].PairedAlnCanIdx == -1) AlignmentVec2[j].Score = 0;

	if (bDebugMode)
	{
		for (i = 0; i != num1; i++)
		{
			if ((j = AlignmentVec1[i].PairedAlnCanIdx) != -1)
				printf("#%d(s=%d) and #%d(s=%d) are pairing\n", i + 1, AlignmentVec1[i].Score, j + 1, AlignmentVec2[j].Score);
		}
	}
}

void CheckPairedFinalAlignments(ReadItem_t& read1, ReadItem_t& read2)
{
	bool bMated;
	int i, j, s;

	//printf("BestIdx1=%d, BestIdx2=%d\n", read1.iBestAlnCanIdx + 1, read2.iBestAlnCanIdx + 1);
	bMated = read1.AlnReportArr[read1.iBestAlnCanIdx].PairedAlnCanIdx == read2.iBestAlnCanIdx ? true : false;
	if (!bMultiHit && bMated) return;
	if (!bMated && read1.score > 0 && read2.score > 0) // identify mated pairs
	{
		for (s = 0, i = 0; i != read1.CanNum; i++)
		{
			if (read1.AlnReportArr[i].AlnScore > 0 && (j = read1.AlnReportArr[i].PairedAlnCanIdx) != -1 && read2.AlnReportArr[j].AlnScore > 0)
			{
				bMated = true;
				if (s < read1.AlnReportArr[i].AlnScore + read2.AlnReportArr[j].AlnScore)
				{
					s = read1.AlnReportArr[i].AlnScore + read2.AlnReportArr[j].AlnScore;
					read1.iBestAlnCanIdx = i; read1.score = read1.AlnReportArr[i].AlnScore;
					read2.iBestAlnCanIdx = j; read2.score = read2.AlnReportArr[j].AlnScore;
				}
			}
		}
	}
	if (bMated)
	{
		for (i = 0; i != read1.CanNum; i++)
		{
			if (read1.AlnReportArr[i].AlnScore != read1.score || ((j = read1.AlnReportArr[i].PairedAlnCanIdx) != -1 && read2.AlnReportArr[j].AlnScore != read2.score))
			{
				read1.AlnReportArr[i].AlnScore = 0;
				read1.AlnReportArr[i].PairedAlnCanIdx = -1;
				continue;
			}
		}
	}
	else // remove all mated info
	{
		for (i = 0; i != read1.CanNum; i++)
		{
			if (read1.AlnReportArr[i].PairedAlnCanIdx != -1) read1.AlnReportArr[i].PairedAlnCanIdx = -1;
			if (read1.AlnReportArr[i].AlnScore > 0 && read1.AlnReportArr[i].AlnScore != read1.score) read1.AlnReportArr[i].AlnScore = 0;
		}
		for (j = 0; j != read2.CanNum; j++)
		{
			if (read2.AlnReportArr[j].PairedAlnCanIdx != -1) read2.AlnReportArr[j].PairedAlnCanIdx = -1;
			if (read2.AlnReportArr[j].AlnScore > 0 && read2.AlnReportArr[j].AlnScore != read2.score) read2.AlnReportArr[j].AlnScore = 0;
		}
	}
}

void UpdateLocalSJMap(AlignmentCandidate_t& Aln, map<pair<int64_t, int64_t>, SpliceJunction_t>& LocalSJMap)
{
	if (Aln.SJtype == -1) return;

	int64_t g1, g2;
	SpliceJunction_t SpliceJunction;
	int i, num = (int)Aln.SeedVec.size();
	map<pair<int64_t, int64_t>, SpliceJunction_t>::iterator iter;

	for (i = 1; i < num; i++)
	{
		if (Aln.SeedVec[i].bAcceptorSite)
		{
			if (Aln.PosDiff < GenomeSize)
			{
				g1 = Aln.SeedVec[i - 1].gPos + Aln.SeedVec[i - 1].gLen;
				g2 = Aln.SeedVec[i].gPos - 1;
			}
			else
			{
				g1 = TwoGenomeSize - Aln.SeedVec[i].gPos;
				g2 = TwoGenomeSize - 1 - (Aln.SeedVec[i - 1].gPos + Aln.SeedVec[i - 1].gLen);
			}
			if ((iter = LocalSJMap.find(make_pair(g1, g2))) != LocalSJMap.end()) iter->second.iCount++;
			else
			{
				SpliceJunction.iCount = 1;
				SpliceJunction.type = Aln.SJtype;
				LocalSJMap.insert(make_pair(make_pair(g1, g2), SpliceJunction));
			}
		}
	}
}

void UpdateGlobalSJMap(map<pair<int64_t, int64_t>, SpliceJunction_t>& LocalSJMap)
{
	map<pair<int64_t, int64_t>, SpliceJunction_t>::iterator iter, ft;

	for (iter = LocalSJMap.begin(); iter != LocalSJMap.end(); iter++)
	{
		ft = SpliceJunctionMap.find(iter->first);
		if (ft != SpliceJunctionMap.end()) ft->second.iCount += iter->second.iCount;
		else SpliceJunctionMap.insert(make_pair(iter->first, iter->second));
	}
}

void *ReadMapping(void *arg)
{
	int i, j, ReadNum;
	ReadItem_t* ReadArr = NULL;
	vector<SeedPair_t> SeedPairVec1, SeedPairVec2;
	map<pair<int64_t, int64_t>, SpliceJunction_t> LocalSJMap;
	vector<AlignmentCandidate_t> AlignmentVec1, AlignmentVec2;

	ReadArr = new ReadItem_t[ReadChunkSize];
	while (true)
	{
		pthread_mutex_lock(&LibraryLock);
		if (gzCompressed) ReadNum = gzGetNextChunk(bSepLibrary, gzReadFileHandler1, gzReadFileHandler2, ReadArr);
		else ReadNum = GetNextChunk(bSepLibrary, ReadFileHandler1, ReadFileHandler2, ReadArr);
		fprintf(stderr, "\r%d %s tags have been processed in %ld seconds...", iTotalReadNum, (bPairEnd? "paired-end":"singled-end"), (time(NULL) - StartProcessTime));
		pthread_mutex_unlock(&LibraryLock);
		
		if (ReadNum == 0) break;
		if (bPairEnd && ReadNum % 2 == 0)
		{
			for (i = 0, j = 1; i != ReadNum; i += 2, j += 2)
			{
				//if (bDebugMode) printf("Mapping paired reads#%d %s (len=%d) and %s (len=%d):\n", i + 1, ReadArr[i].header + 1, ReadArr[i].rlen, ReadArr[j].header + 1, ReadArr[j].rlen);

				SeedPairVec1 = IdentifySeedPairs(ReadArr[i].rlen, ReadArr[i].EncodeSeq); //if (bDebugMode) ShowSeedInfo(SeedPairVec1);
				AlignmentVec1 = GenerateAlignmentCandidate(ReadArr[i].rlen, SeedPairVec1);

				SeedPairVec2 = IdentifySeedPairs(ReadArr[j].rlen, ReadArr[j].EncodeSeq); //if (bDebugMode) ShowSeedInfo(SeedPairVec2);
				AlignmentVec2 = GenerateAlignmentCandidate(ReadArr[j].rlen, SeedPairVec2);

				//if (bDebugMode) ShowAlignmentCandidateInfo(true, ReadArr[i].header+1, AlignmentVec1), ShowAlignmentCandidateInfo(false, ReadArr[j].header+1, AlignmentVec2);
				if (CheckPairedAlignmentCandidates(AlignmentVec1, AlignmentVec2))
				{
					RemoveUnMatedAlignmentCandidates(AlignmentVec1, AlignmentVec2);
				}
				RemoveRedundantCandidates(AlignmentVec1); RemoveRedundantCandidates(AlignmentVec2);
				
				GenMappingReport(true,  ReadArr[i], AlignmentVec1);
				GenMappingReport(false, ReadArr[j], AlignmentVec2);

				CheckPairedFinalAlignments(ReadArr[i], ReadArr[j]);

				SetPairedAlignmentFlag(ReadArr[i], ReadArr[j]);
				EvaluateMAPQ(ReadArr[i]); EvaluateMAPQ(ReadArr[j]);

				if (ReadArr[i].mapq == Max_MAPQ) UpdateLocalSJMap(AlignmentVec1[ReadArr[i].iBestAlnCanIdx], LocalSJMap);
				if (ReadArr[j].mapq == Max_MAPQ) UpdateLocalSJMap(AlignmentVec2[ReadArr[j].iBestAlnCanIdx], LocalSJMap);

				//if (bDebugMode) printf("\nEnd of mapping for read#%s\n%s\n", ReadArr[i].header, string().assign(100, '=').c_str());
			}
		}
		else
		{
			for (i = 0; i != ReadNum; i++)
			{
				//if (bDebugMode) printf("Mapping single read#%d %s (len=%d):\n", i + 1, ReadArr[i].header + 1, ReadArr[i].rlen);
				//fprintf(stdout, "%s\n", ReadArr[i].header + 1); fflush(output);

				SeedPairVec1 = IdentifySeedPairs(ReadArr[i].rlen, ReadArr[i].EncodeSeq); //if (bDebugMode) ShowSeedInfo(SeedPairVec1);
				AlignmentVec1 = GenerateAlignmentCandidate(ReadArr[i].rlen, SeedPairVec1);
				RemoveRedundantCandidates(AlignmentVec1); if (bDebugMode) ShowAlignmentCandidateInfo(1, ReadArr[i].header+1, AlignmentVec1);
				GenMappingReport(true, ReadArr[i], AlignmentVec1);
				SetSingleAlignmentFlag(ReadArr[i]); EvaluateMAPQ(ReadArr[i]);

				if (SJFileName != NULL && ReadArr[i].mapq == Max_MAPQ) UpdateLocalSJMap(AlignmentVec1[ReadArr[i].iBestAlnCanIdx], LocalSJMap);

				//if (bDebugMode) printf("\nEnd of mapping for read#%s\n%s\n", ReadArr[i].header, string().assign(100, '=').c_str());
			}
		}
		pthread_mutex_lock(&OutputLock);
		iTotalReadNum += ReadNum;
		if (bPairEnd && ReadNum %2 == 0) for (i = 0, j = 1; i != ReadNum; i += 2, j += 2) OutputPairedAlignments(ReadArr[i], ReadArr[j]);
		else for (i = 0; i != ReadNum; i++) OutputSingledAlignments(ReadArr[i]);
		pthread_mutex_unlock(&OutputLock);

		for (i = 0; i != ReadNum; i++)
		{
			delete[] ReadArr[i].header;
			delete[] ReadArr[i].seq;
			delete[] ReadArr[i].EncodeSeq;
			delete[] ReadArr[i].AlnReportArr;
		}
	}
	delete[] ReadArr;

	pthread_mutex_lock(&OutputLock);
	UpdateGlobalSJMap(LocalSJMap);
	pthread_mutex_unlock(&OutputLock);

	return (void*)(1);
}

int AbsLoc2ChrLoc(int64_t& g1, int64_t& g2)
{
	int ChrIdx;
	map<int64_t, int>::iterator iter;

	iter = ChrLocMap.lower_bound(g1); ChrIdx = iter->second;
	g1 = g1 + 1 - ChromosomeVec[ChrIdx].FowardLocation;
	g2 = g2 + 1 - ChromosomeVec[ChrIdx].FowardLocation;
	return ChrIdx;
}

int OutputSpliceJunctions()
{
	int ChrIdx;
	int64_t g1, g2;
	FILE *SJFile = fopen(SJFileName, "w");

	for (map<pair<int64_t, int64_t>, SpliceJunction_t>::iterator iter = SpliceJunctionMap.begin(); iter != SpliceJunctionMap.end(); iter++)
	{
		g1 = iter->first.first; g2 = iter->first.second;
		ChrIdx = AbsLoc2ChrLoc(g1, g2); //SJtypeNum[iter->second.type]++;
		fprintf(SJFile, "%s\t%ld\t%ld\t%d\n", ChromosomeVec[ChrIdx].name, g1, g2, iter->second.iCount);
	}
	fclose(SJFile);

	return (int)SpliceJunctionMap.size();
}

void Mapping()
{
	int i;

	if (gzCompressed)
	{
		gzReadFileHandler1 = gzopen(ReadFileName, "rb");
		if (ReadFileName2 != NULL)
		{
			bSepLibrary = bPairEnd = true;
			gzReadFileHandler2 = gzopen(ReadFileName2, "rb");
		}
	}
	else
	{
		ReadFileHandler1 = fopen(ReadFileName, "r");
		if (ReadFileName2 != NULL)
		{
			bSepLibrary = bPairEnd = true;
			ReadFileHandler2 = fopen(ReadFileName2, "r");
		}
	}

	if (OutputFileName != NULL)
	{
		if (OutputFileFormat == 0) output = fopen(OutputFileName, "w");
		else gzOutput = gzopen(OutputFileName, "wb");
	}

	if (bDebugMode) iThreadNum = 1;
	else
	{
		int len;
		char buffer[1024];
		len = sprintf(buffer, "@PG\tPN:Dart\tVN:%s\n", VersionStr);

		if (OutputFileFormat == 0) fprintf(output, "%s", buffer);
		else gzwrite(gzOutput, buffer, len);

		for (i = 0; i < iChromsomeNum; i++)
		{
			len = sprintf(buffer, "@SQ\tSN:%s\tLN:%ld\n", ChromosomeVec[i].name, ChromosomeVec[i].len);

			if (OutputFileFormat == 0) fprintf(output, "%s", buffer);
			else gzwrite(gzOutput, buffer, len);
		}
	}

	//iThreadNum = 1;
	StartProcessTime = time(NULL);
	pthread_t *ThreadArr = new pthread_t[iThreadNum];
	for (i = 0; i < iThreadNum; i++) pthread_create(&ThreadArr[i], NULL, ReadMapping, NULL);
	for (i = 0; i < iThreadNum; i++) pthread_join(ThreadArr[i], NULL);
	delete[] ThreadArr;

	if (gzCompressed) gzclose(gzReadFileHandler1);
	else fclose(ReadFileHandler1);
	if (ReadFileName2 != NULL)
	{
		if (gzCompressed) gzclose(gzReadFileHandler2);
		else fclose(ReadFileHandler2);
	}
	fprintf(stderr, "\rAll the %d %s reads have been processed in %lld seconds.\n", iTotalReadNum, (bPairEnd? "paired-end":"single-end"), (long long)(time(NULL) - StartProcessTime));

	if (OutputFileName != NULL)
	{
		if (OutputFileFormat == 0) fclose(output);
		else if (OutputFileFormat == 1) gzclose(gzOutput);
		//else output = fopen(OutputFileName, "wb");
	}

	if(iTotalReadNum > 0)
	{
		if (bPairEnd) fprintf(stderr, "\t# of total mapped reads = %d (sensitivity = %.2f%%)\n\t# of paired sequences = %d (%.2f%%)\n", iTotalReadNum - iUnMapped, (int)(10000 * (1.0*(iTotalReadNum - iUnMapped) / iTotalReadNum) + 0.5) / 100.0, iPaired, (int)(10000 * (1.0*iPaired / iTotalReadNum) + 0.5) / 100.0);
		else fprintf(stderr, "\t# of total mapped reads = %d (sensitivity = %.2f%%)\n", iTotalReadNum - iUnMapped, (int)(10000 * (1.0*(iTotalReadNum - iUnMapped) / iTotalReadNum) + 0.5) / 100.0);
		fprintf(stderr, "\t# of unique mapped reads = %d (%.2f%%)\n", iUniqueMapped, (int)(10000 * (1.0*iUniqueMapped / iTotalReadNum) + 0.5) / 100.0);
		if (!bUnique) fprintf(stderr, "\t# of multiple mapped reads = %d (%.2f%%)\n", (iTotalReadNum - iUnMapped - iUniqueMapped), (int)(10000 * (1.0*(iTotalReadNum - iUnMapped - iUniqueMapped) / iTotalReadNum) + 0.5) / 100.0);
		fprintf(stderr, "\t# of unmapped reads = %d (%.2f%%)\n", iUnMapped, (int)(10000 * (1.0*iUnMapped / iTotalReadNum) + 0.5) / 100.0);

		i = OutputSpliceJunctions();
		fprintf(stderr, "\t# of splice junctions = %d (file: %s)\n\n", i, SJFileName);
	}
}
